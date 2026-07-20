"""
bmpclient/concurrent_io.py — 统一并发读写引擎。

为上层系统（如 sglang / vllm 的 KV cache 卸载）提供抽象的"地址 + buffer"并发
读写能力。本模块不含任何 KV cache 语义（无 token/page/layer 概念），只基于
UMMLib（ctypes）构建；ctypes CDLL 调用会释放 GIL，因此多线程 umm_read/umm_write
可以真正并行。

用法示例（sglang hisparse 式卸载：一次提交整批离散写）：

    engine = ConcurrentIOEngine(client.lib, num_workers=8, num_queues=4)

    reqs = [IORequest(IOAddress.from_descriptor(desc, off, PAGE),
                      memoryview(host_buf)[i*PAGE:(i+1)*PAGE])
            for i, off in enumerate(slot_offsets)]
    h = engine.submit_write(reqs)      # 立即返回
    ...                                # 做别的事（如调度下一个 batch）
    h.result()                         # 需要时等待

buffer 生命周期约定：同步 API（read_batch/write_batch）返回时数据已就位；
异步 API（submit_*）在对应 IOHandle 完成前，调用方不得复用/释放 buffer。
"""

import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Any, Callable, List, Optional, Union

from bmpclient.umm_client import ChunkDescriptor, UMMLib

__all__ = ["IOAddress", "IORequest", "IOHandle", "ConcurrentIOEngine"]


# ========================================================================
# 数据模型
# ========================================================================


@dataclass(frozen=True)
class IOAddress:
    """
    统一 I/O 地址：UMM Chunk 内的一段字节区间。

    user_size 是所属 chunk 的总大小（字节）。真实 libumm 的 umm_read/umm_write
    会用 desc->user_size 做边界检查并拒绝 user_size==0 的描述符，
    因此手工构造 IOAddress 时必须填入正确的 user_size；
    from_descriptor / from_block 会自动带上。
    """
    chunk_id: int
    base_gpa: int
    offset: int            # chunk 内字节偏移
    length: int            # 字节数
    user_size: int = 0     # 所属 chunk 总大小（真实 libumm 边界检查需要）

    @staticmethod
    def from_descriptor(desc: ChunkDescriptor, offset: int, length: int) -> "IOAddress":
        """从 UMM ChunkDescriptor 构造（自动携带 user_size）。"""
        return IOAddress(
            chunk_id=desc.chunk_id,
            base_gpa=desc.base_gpa,
            offset=offset,
            length=length,
            user_size=desc.user_size,
        )

    @staticmethod
    def from_block(block, offset: int = 0, length: Optional[int] = None) -> "IOAddress":
        """从 allocator.Block 构造（block 有 chunk_id/offset/size/gpa/chunk_size 字段）。"""
        if length is None:
            length = block.size - offset
        return IOAddress(
            chunk_id=block.chunk_id,
            base_gpa=block.gpa,
            offset=block.offset + offset,
            length=length,
            user_size=getattr(block, "chunk_size", 0),
        )


@dataclass
class IORequest:
    """一条读写请求：地址 + 用户 buffer。"""
    address: IOAddress
    buffer: memoryview   # read: 可写视图；write: 可读视图
    opaque: Any = None   # 调用方透传上下文（如 request_id），结果原样带回

    def __post_init__(self):
        # 允许直接传 bytes/bytearray 等 buffer 协议对象，统一收敛为 memoryview
        if not isinstance(self.buffer, memoryview):
            self.buffer = memoryview(self.buffer)


# ========================================================================
# IOHandle：异步句柄（future 语义）
# ========================================================================


class IOHandle:
    """
    对一个已提交 I/O（单条或一批）的引用。

    每请求独立成败：result() 抛首个失败的异常；results() 返回逐条结果
    （int 或 BaseException），供细粒度检查。
    """

    def __init__(self, requests: List[IORequest]):
        self._requests = list(requests)
        self._results: List[Union[int, BaseException, None]] = [None] * len(requests)
        self._remaining = len(requests)
        self._cond = threading.Condition()
        self._callbacks: List[Callable[["IOHandle"], None]] = []

    # ---- 引擎内部接口 ----

    def _set_result(self, index: int, value: Union[int, BaseException]) -> None:
        """记录第 index 条请求的结果；全部完成时唤醒等待者并触发回调。"""
        self._set_many([(index, value)])

    def _set_many(self, pairs: List[tuple]) -> None:
        """
        批量记录多条请求的结果（一次锁获取），全部完成时唤醒等待者并触发回调。
        注意：以组为粒度回写意味着 results() 在句柄完成前只能观察到
        组级进度；done()/result() 语义不变。
        """
        with self._cond:
            for index, value in pairs:
                self._results[index] = value
            self._remaining -= len(pairs)
            if self._remaining > 0:
                return
            callbacks = list(self._callbacks)
            self._cond.notify_all()
        # 回调在锁外执行，避免回调里调用 handle 方法造成死锁
        for cb in callbacks:
            try:
                cb(self)
            except Exception:
                pass  # 回调异常不影响引擎与其他回调

    # ---- 公开接口 ----

    @property
    def requests(self) -> List[IORequest]:
        """原请求（含 opaque）。"""
        return list(self._requests)

    def done(self) -> bool:
        with self._cond:
            return self._remaining == 0

    def wait(self, timeout: Optional[float] = None) -> bool:
        """阻塞直到整批完成；返回是否在 timeout 内完成。"""
        with self._cond:
            if timeout is None:
                while self._remaining > 0:
                    self._cond.wait()
                return True
            deadline = time.monotonic() + timeout
            while self._remaining > 0:
                left = deadline - time.monotonic()
                if left <= 0:
                    return False
                self._cond.wait(left)
            return True

    def result(self, timeout: Optional[float] = None) -> List[int]:
        """
        阻塞直到完成，返回每条请求的字节数列表（与提交顺序一致）。
        任一请求失败则抛该请求的异常（首个失败）。
        """
        if not self.wait(timeout):
            raise TimeoutError("IOHandle.result 等待超时")
        for r in self._results:
            if isinstance(r, BaseException):
                raise r
        return list(self._results)

    def results(self, timeout: Optional[float] = None) -> List[Union[int, BaseException]]:
        """阻塞直到完成，逐条返回结果：成功为字节数 int，失败为该条的异常对象。"""
        if not self.wait(timeout):
            raise TimeoutError("IOHandle.results 等待超时")
        return list(self._results)

    def exception(self, timeout: Optional[float] = None) -> Optional[BaseException]:
        """返回首个失败的异常；全部成功返回 None。"""
        if not self.wait(timeout):
            raise TimeoutError("IOHandle.exception 等待超时")
        for r in self._results:
            if isinstance(r, BaseException):
                return r
        return None

    def add_done_callback(self, fn: Callable[["IOHandle"], None]) -> None:
        """整批完成时回调 fn(handle)；若已完成则立即调用。"""
        with self._cond:
            if self._remaining > 0:
                self._callbacks.append(fn)
                return
        fn(self)


# ========================================================================
# ConcurrentIOEngine：并发读写引擎
# ========================================================================


class ConcurrentIOEngine:
    """
    统一并发读写引擎。

    :param lib: UMMLib 实例（ctypes 封装；调用释放 GIL，可真正并行）
    :param num_workers: num_queues == 1 时共享线程池的工作线程数
    :param num_queues: >1 时维护 num_queues 个单线程执行器，请求按
        chunk_id % num_queues 路由，同一 chunk 的 I/O 严格 FIFO 保序，
        不同 chunk 并行；==1 时共享一个 num_workers 线程池，不保证跨请求顺序
    :param max_inflight: 背压：在途请求上限（按条计数），超限 submit 阻塞

    引擎可被多线程共享提交；内部绝不分配与数据等大的临时 buffer，
    读写均通过 UMMLib.read_into / write_from 直接落在调用方 buffer 上。
    """

    def __init__(
        self,
        lib: UMMLib,
        num_workers: int = 4,
        num_queues: int = 1,
        max_inflight: Optional[int] = None,
    ):
        if num_workers < 1:
            raise ValueError("num_workers 必须 >= 1")
        if num_queues < 1:
            raise ValueError("num_queues 必须 >= 1")
        if max_inflight is not None and max_inflight < 1:
            raise ValueError("max_inflight 必须 >= 1")

        self._lib = lib
        self._num_workers = num_workers
        self._num_queues = num_queues
        if num_queues > 1:
            # 每队列单线程：同桶内严格 FIFO
            self._executors = [
                ThreadPoolExecutor(max_workers=1, thread_name_prefix=f"conc-io-q{i}")
                for i in range(num_queues)
            ]
        else:
            self._executors = [
                ThreadPoolExecutor(max_workers=num_workers, thread_name_prefix="conc-io")
            ]
        self._inflight_sem = threading.Semaphore(max_inflight) if max_inflight else None

        self._state_lock = threading.Lock()   # 保护 _closed
        self._closed = False
        self._stats_lock = threading.Lock()   # 保护以下计数器
        self._submitted = 0
        self._completed = 0
        self._failed = 0
        self._inflight = 0

    # ------------------------------------------------------------------
    # 同步批量（薄封装 = submit + result）
    # ------------------------------------------------------------------

    def read_batch(self, requests: List[IORequest]) -> List[int]:
        """同步批量读：返回每条读取字节数；任一失败抛首个异常。"""
        return self.submit_read(requests).result()

    def write_batch(self, requests: List[IORequest]) -> List[int]:
        """同步批量写：返回每条写入字节数；任一失败抛首个异常。"""
        return self.submit_write(requests).result()

    # ------------------------------------------------------------------
    # 异步提交
    # ------------------------------------------------------------------

    def submit_read(self, requests: List[IORequest]) -> IOHandle:
        """异步提交一批读请求，立即返回 IOHandle。"""
        return self._submit([(req, True) for req in requests])

    def submit_write(self, requests: List[IORequest]) -> IOHandle:
        """异步提交一批写请求，立即返回 IOHandle。"""
        return self._submit([(req, False) for req in requests])

    def submit_mixed(
        self, reads: List[IORequest], writes: List[IORequest]
    ) -> IOHandle:
        """异步提交混合读写；结果顺序为 reads 后接 writes。"""
        return self._submit(
            [(req, True) for req in reads] + [(req, False) for req in writes]
        )

    # ------------------------------------------------------------------
    # 句柄组合
    # ------------------------------------------------------------------

    @staticmethod
    def wait_all(handles: List[IOHandle], timeout: Optional[float] = None) -> None:
        """等待所有 handle 完成；超时抛 TimeoutError（不传播 handle 内的 I/O 异常）。"""
        deadline = None if timeout is None else time.monotonic() + timeout
        for h in handles:
            left = None if deadline is None else max(0.0, deadline - time.monotonic())
            if not h.wait(left):
                raise TimeoutError("wait_all 等待超时")

    @staticmethod
    def wait_any(handles: List[IOHandle], timeout: Optional[float] = None) -> IOHandle:
        """返回首个完成的 handle；超时抛 TimeoutError。"""
        if not handles:
            raise ValueError("handles 不能为空")
        event = threading.Event()
        for h in handles:
            h.add_done_callback(lambda _h: event.set())
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            for h in handles:
                if h.done():
                    return h
            if deadline is not None:
                left = deadline - time.monotonic()
                if left <= 0:
                    raise TimeoutError("wait_any 等待超时")
                event.wait(left)
            else:
                event.wait()
            event.clear()

    # ------------------------------------------------------------------
    # 状态与生命周期
    # ------------------------------------------------------------------

    def stats(self) -> dict:
        """返回 submitted/completed/failed 计数与当前在途数。"""
        with self._stats_lock:
            return {
                "submitted": self._submitted,
                "completed": self._completed,
                "failed": self._failed,
                "inflight": self._inflight,
            }

    def close(self, wait: bool = True) -> None:
        """关闭引擎：不再接受新请求；wait=True 时等待在途请求完成。"""
        with self._state_lock:
            if self._closed:
                return
            self._closed = True
        for ex in self._executors:
            ex.shutdown(wait=wait)

    def __enter__(self) -> "ConcurrentIOEngine":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    # ------------------------------------------------------------------
    # 内部实现
    # ------------------------------------------------------------------

    def _executor_for(self, chunk_id: int) -> ThreadPoolExecutor:
        if self._num_queues > 1:
            return self._executors[chunk_id % self._num_queues]
        return self._executors[0]

    def _submit(self, items: List[tuple]) -> IOHandle:
        with self._state_lock:
            if self._closed:
                raise RuntimeError("ConcurrentIOEngine 已关闭")
        handle = IOHandle([req for req, _ in items])

        # 分片提交：不是每条请求一个任务，而是每个执行队列一个任务，
        # worker 在任务内顺序执行该片的所有请求。
        # 对 4KB 级小 IO，逐条 submit 的任务分发开销（~10us/条）远超
        # memcpy 本身（~0.2us/条），会把并发做成负收益；分片后任务数
        # 从 N 降到队列数，Python 开销被摊薄到可忽略。
        #
        # 分片键：
        #   num_queues > 1 —— chunk_id % num_queues（同 chunk 同片，
        #                     单线程队列内保持 FIFO 保序）；
        #   num_queues == 1 —— 按 num_workers 轮询分片（跨片并行，
        #                     不保证跨请求顺序，与共享池语义一致）。
        groups: List[List[tuple]] = []
        group_of = {}
        for idx, (req, is_read) in enumerate(items):
            # 背压：在途请求达上限时在此阻塞，直到有请求完成释放许可
            if self._inflight_sem is not None:
                self._inflight_sem.acquire()
            with self._stats_lock:
                self._submitted += 1
                self._inflight += 1

            if self._num_queues > 1:
                key = req.address.chunk_id % self._num_queues
            else:
                key = idx % self._num_workers
            pos = group_of.get(key)
            if pos is None:
                group_of[key] = len(groups)
                groups.append([])
                pos = group_of[key]
            groups[pos].append((idx, req, is_read))

        for key, pos in group_of.items():
            group = groups[pos]
            executor = (
                self._executors[key % len(self._executors)]
                if self._num_queues > 1
                else self._executors[0]
            )
            try:
                executor.submit(self._run_group, handle, group)
            except BaseException as e:
                # 与 close() 竞争导致无法调度：整组按失败收尾，句柄仍可用
                for idx, req, is_read in group:
                    if self._inflight_sem is not None:
                        self._inflight_sem.release()
                    with self._stats_lock:
                        self._submitted -= 1
                        self._inflight -= 1
                    handle._set_result(idx, e)
        return handle

    def _run_group(self, handle: IOHandle, group: List[tuple]) -> None:
        """
        worker 任务：顺序执行本组所有请求（组内次序即提交次序）。
        结果与统计按组聚合回写（每组一次锁），把小 IO 的簿记开销摊薄。
        """
        results: List[tuple] = []
        n_ok = 0
        n_fail = 0
        for idx, req, is_read in group:
            try:
                n = self._execute_one(req, is_read)
            except BaseException as e:
                n_fail += 1
                results.append((idx, e))
            else:
                n_ok += 1
                results.append((idx, n))
        with self._stats_lock:
            self._completed += n_ok
            self._failed += n_fail
            self._inflight -= len(group)
        if self._inflight_sem is not None:
            for _ in group:
                self._inflight_sem.release()
        handle._set_many(results)

    def _execute_one(self, req: IORequest, is_read: bool) -> int:
        addr = req.address
        if addr.offset < 0:
            raise ValueError(f"offset 必须非负: {addr.offset}")
        if addr.length < 0:
            raise ValueError(f"length 必须非负: {addr.length}")
        buf = req.buffer
        if buf.nbytes < addr.length:
            raise ValueError(
                f"buffer 不足: buffer.nbytes={buf.nbytes} < length={addr.length}"
            )
        if addr.length == 0:
            return 0
        # 前缀语义：buffer 大于 length 时只读写前 length 字节；
        # memoryview 切片不拷贝数据，数据直接落在调用方 buffer 上
        view = buf if buf.nbytes == addr.length else buf[: addr.length]
        desc = ChunkDescriptor(
            chunk_id=addr.chunk_id,
            base_gpa=addr.base_gpa,
            user_size=addr.user_size,  # 真实 libumm 用它做边界检查（拒绝 0）
        )
        if is_read:
            return self._lib.read_into(desc, addr.offset, view)
        return self._lib.write_from(desc, addr.offset, view)
