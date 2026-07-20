# -*- coding: utf-8 -*-
"""
tests/test_concurrent_io.py — concurrent_io 并发读写模块测试。

用 FakeUMMLib（内存 dict 模拟 chunk 存储 + threading 安全）替代真实 libumm.so，
复用 UMMLib 的全部方法（含零拷贝 read_into/write_from），
ctypes 指针层面的行为与真实 C 库一致。
"""

import ctypes
import os
import random
import sys
import threading
import time
import unittest

# ------------------------------------------------------------------------
# 导入路径引导：仓库目录名必须是 bmpclient 才能 `import bmpclient`；
# git worktree 等目录名不符的场景下，手动把仓库目录注册为 bmpclient 包。
# ------------------------------------------------------------------------
_REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_PARENT_DIR = os.path.dirname(_REPO_DIR)
if _PARENT_DIR not in sys.path:
    sys.path.insert(0, _PARENT_DIR)
try:
    import bmpclient  # noqa: F401
except ImportError:
    import importlib.util

    _spec = importlib.util.spec_from_file_location(
        "bmpclient",
        os.path.join(_REPO_DIR, "__init__.py"),
        submodule_search_locations=[_REPO_DIR],
    )
    _mod = importlib.util.module_from_spec(_spec)
    sys.modules["bmpclient"] = _mod
    _spec.loader.exec_module(_mod)

from bmpclient import ConcurrentIOEngine, IOAddress, IOHandle, IORequest
from bmpclient.allocator import Block
from bmpclient.concurrent_io import IOHandle as IOHandleDirect  # 直导路径 sanity
from bmpclient.umm_client import ChunkDescriptor, UMMLib

PAGE = 4096
_DESC_PTR = ctypes.POINTER(ChunkDescriptor)


# ========================================================================
# FakeUMMLib 已提升为包级测试工具 bmpclient.testing（demo 场景 C 共用）
# ========================================================================

from bmpclient.testing import FakeUMMLib, _FakeCLib  # noqa: F401



def _addr(desc, offset, length):
    return IOAddress.from_descriptor(desc, offset, length)


def _pattern(seed_byte: int, n: int) -> bytes:
    """确定性测试数据。"""
    rng = random.Random(seed_byte)
    return rng.randbytes(n)


# ========================================================================
# 测试点 1：单条 read_into/write_from 零拷贝正确性
# ========================================================================


class TestZeroCopy(unittest.TestCase):
    def setUp(self):
        self.lib = FakeUMMLib()
        self.desc = self.lib.alloc(PAGE * 2)

    def test_write_from_read_into_roundtrip(self):
        data = _pattern(1, PAGE)
        n = self.lib.write_from(self.desc, 0, data)
        self.assertEqual(n, PAGE)

        buf = bytearray(PAGE)
        n = self.lib.read_into(self.desc, 0, buf)
        self.assertEqual(n, PAGE)
        self.assertEqual(bytes(buf), data)

        # 带偏移写后读回一致
        part = _pattern(2, 512)
        self.lib.write_from(self.desc, PAGE, part)
        buf2 = bytearray(512)
        self.assertEqual(self.lib.read_into(self.desc, PAGE, buf2), 512)
        self.assertEqual(bytes(buf2), part)

    def test_zero_copy_no_temp_buffer(self):
        """read/write 收到的数据指针必须就是调用方 buffer 的地址（无临时拷贝）。"""
        data = _pattern(3, PAGE)
        self.lib.write_from(self.desc, 0, data)
        bytes_addr = ctypes.cast(ctypes.c_char_p(data), ctypes.c_void_p).value
        self.assertEqual(self.lib.fake.last_write_ptr, bytes_addr)

        buf = bytearray(PAGE)
        self.lib.read_into(self.desc, 0, buf)
        buf_addr = ctypes.addressof(ctypes.c_char.from_buffer(buf))
        self.assertEqual(self.lib.fake.last_read_ptr, buf_addr)

        # bytearray / memoryview 写出同样是零拷贝
        wbuf = bytearray(_pattern(4, PAGE))
        self.lib.write_from(self.desc, 0, memoryview(wbuf))
        wbuf_addr = ctypes.addressof(ctypes.c_char.from_buffer(wbuf))
        self.assertEqual(self.lib.fake.last_write_ptr, wbuf_addr)

    def test_read_into_accepts_ctypes_array(self):
        data = _pattern(5, 256)
        self.lib.write_from(self.desc, 0, data)
        c_arr = (ctypes.c_char * 256)()
        n = self.lib.read_into(self.desc, 0, c_arr)
        self.assertEqual(n, 256)
        self.assertEqual(c_arr.raw, data)

    def test_read_into_readonly_raises(self):
        with self.assertRaises(ValueError):
            self.lib.read_into(self.desc, 0, b"\x00" * 16)
        with self.assertRaises(ValueError):
            self.lib.read_into(self.desc, 0, memoryview(b"\x00" * 16))

    def test_empty_buffer_returns_zero(self):
        self.assertEqual(self.lib.read_into(self.desc, 0, bytearray()), 0)
        self.assertEqual(self.lib.write_from(self.desc, 0, b""), 0)

    def test_write_from_memoryview_of_bytes_zero_copy(self):
        """memoryview 包住 bytes（完整视图）时仍应零拷贝：直接引用底层 bytes。"""
        data = _pattern(6, PAGE)
        n = self.lib.write_from(self.desc, 0, memoryview(data))
        self.assertEqual(n, PAGE)
        bytes_addr = ctypes.cast(ctypes.c_char_p(data), ctypes.c_void_p).value
        self.assertEqual(self.lib.fake.last_write_ptr, bytes_addr)


# ========================================================================
# 测试点 2：read_batch/write_batch 大批量数据一致性
# ========================================================================


class TestBatchConsistency(unittest.TestCase):
    def test_large_batch(self):
        n_reqs = 2048
        n_chunks = 8
        pages_per_chunk = n_reqs // n_chunks   # 每 chunk 256 个不相交 4KB 槽位
        lib = FakeUMMLib()
        descs = [lib.alloc(pages_per_chunk * PAGE) for _ in range(n_chunks)]
        payloads = [_pattern(i, PAGE) for i in range(n_reqs)]

        def slot_addr(i):
            return _addr(descs[i // pages_per_chunk], (i % pages_per_chunk) * PAGE, PAGE)

        with ConcurrentIOEngine(lib, num_workers=8) as engine:
            wreqs = [
                IORequest(slot_addr(i), payloads[i], opaque=i)
                for i in range(n_reqs)
            ]
            res = engine.write_batch(wreqs)
            self.assertEqual(res, [PAGE] * n_reqs)

            rbufs = [bytearray(PAGE) for _ in range(n_reqs)]
            rreqs = [IORequest(slot_addr(i), rbufs[i]) for i in range(n_reqs)]
            res = engine.read_batch(rreqs)
            self.assertEqual(res, [PAGE] * n_reqs)

        for i in range(n_reqs):
            self.assertEqual(bytes(rbufs[i]), payloads[i], f"第 {i} 条数据不一致")

        stats = engine.stats()
        self.assertEqual(stats["submitted"], n_reqs * 2)
        self.assertEqual(stats["completed"], n_reqs * 2)
        self.assertEqual(stats["failed"], 0)
        self.assertEqual(stats["inflight"], 0)


# ========================================================================
# 测试点 3：异步 submit 立即返回 + result 正确 + callback 被调用
# ========================================================================


class TestAsyncSubmit(unittest.TestCase):
    def test_submit_returns_immediately_and_callback_fires(self):
        lib = FakeUMMLib(io_delay=0.05)
        desc = lib.alloc(PAGE)
        payload = _pattern(7, PAGE)

        with ConcurrentIOEngine(lib, num_workers=2) as engine:
            t0 = time.monotonic()
            h = engine.submit_write([IORequest(_addr(desc, 0, PAGE), payload, opaque="w1")])
            elapsed = time.monotonic() - t0
            self.assertLess(elapsed, 0.04, "submit 必须立即返回，不等待 I/O 完成")
            self.assertFalse(h.done(), "I/O 未完成时 done() 应为 False")

            fired = threading.Event()
            seen = []
            h.add_done_callback(lambda hh: (seen.append(hh), fired.set()))

            self.assertEqual(h.result(), [PAGE])
            self.assertTrue(h.done())
            self.assertTrue(fired.wait(1.0), "完成回调必须被调用")
            self.assertIs(seen[0], h)
            # opaque 原样带回
            self.assertEqual(h.requests[0].opaque, "w1")

            # 已完成的 handle 再注册回调：立即调用
            fired2 = threading.Event()
            h.add_done_callback(lambda hh: fired2.set())
            self.assertTrue(fired2.is_set())

            # 读回校验
            rbuf = bytearray(PAGE)
            self.assertEqual(engine.submit_read([IORequest(_addr(desc, 0, PAGE), rbuf)]).result(), [PAGE])
            self.assertEqual(bytes(rbuf), payload)

    def test_result_timeout(self):
        lib = FakeUMMLib(io_delay=0.2)
        desc = lib.alloc(PAGE)
        with ConcurrentIOEngine(lib, num_workers=1) as engine:
            h = engine.submit_read([IORequest(_addr(desc, 0, 16), bytearray(16))])
            with self.assertRaises(TimeoutError):
                h.result(timeout=0.05)
            self.assertFalse(h.wait(timeout=0.01))
            self.assertEqual(h.result(timeout=2.0), [16])


# ========================================================================
# 测试点 4：8 线程同时提交不同 chunk，无数据串扰
# ========================================================================


class TestConcurrentSubmitters(unittest.TestCase):
    def test_eight_threads_no_crosstalk(self):
        n_threads = 8
        rounds = 8
        lib = FakeUMMLib()
        descs = [lib.alloc(PAGE) for _ in range(n_threads)]
        payloads = [_pattern(100 + i, PAGE) for i in range(n_threads)]
        errors = []

        with ConcurrentIOEngine(lib, num_workers=8) as engine:
            def worker(i):
                try:
                    addr = _addr(descs[i], 0, PAGE)
                    for _ in range(rounds):
                        engine.write_batch([IORequest(addr, payloads[i])])
                        buf = bytearray(PAGE)
                        engine.read_batch([IORequest(addr, buf)])
                        if bytes(buf) != payloads[i]:
                            errors.append(f"thread {i}: 数据串扰")
                            return
                except Exception as e:  # noqa: BLE001
                    errors.append(f"thread {i}: {e!r}")

            threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_threads)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()

        self.assertEqual(errors, [])
        # 最终每个 chunk 的内容仍是各自 payload
        for i in range(n_threads):
            self.assertEqual(lib.read(descs[i], 0, PAGE), payloads[i])


# ========================================================================
# 测试点 5：num_queues 保序——同一 chunk 写 A 再写 B 覆盖同区间，最终读回为 B
# ========================================================================


class TestNumQueuesOrdering(unittest.TestCase):
    def test_same_chunk_fifo(self):
        lib = FakeUMMLib(io_delay=0.002)
        desc = lib.alloc(PAGE)
        a = b"A" * PAGE
        b = b"B" * PAGE
        rounds = 10

        with ConcurrentIOEngine(lib, num_workers=4, num_queues=4) as engine:
            addr = _addr(desc, 0, PAGE)
            for _ in range(rounds):
                h1 = engine.submit_write([IORequest(addr, a)])
                h2 = engine.submit_write([IORequest(addr, b)])
                ConcurrentIOEngine.wait_all([h1, h2])
                # 同一 chunk 严格 FIFO：A 先落盘，B 后落盘
                self.assertEqual(engine.read_batch([IORequest(addr, bytearray(PAGE))]), [PAGE])
                buf = bytearray(PAGE)
                engine.read_batch([IORequest(addr, buf)])
                self.assertEqual(bytes(buf), b, "同 chunk FIFO 保序被破坏")

    def test_different_chunks_are_parallel(self):
        """不同 chunk 路由到不同队列，互不阻塞（提交顺序不影响各自成败）。"""
        lib = FakeUMMLib(io_delay=0.01)
        n_chunks = 4
        descs = [lib.alloc(PAGE) for _ in range(n_chunks)]
        payloads = [_pattern(200 + i, PAGE) for i in range(n_chunks)]

        with ConcurrentIOEngine(lib, num_workers=4, num_queues=2) as engine:
            handles = [
                engine.submit_write([IORequest(_addr(descs[i], 0, PAGE), payloads[i])])
                for i in range(n_chunks)
            ]
            ConcurrentIOEngine.wait_all(handles, timeout=10.0)
            for i, h in enumerate(handles):
                self.assertEqual(h.result(), [PAGE])
            for i in range(n_chunks):
                buf = bytearray(PAGE)
                engine.read_batch([IORequest(_addr(descs[i], 0, PAGE), buf)])
                self.assertEqual(bytes(buf), payloads[i])


# ========================================================================
# 测试点 6：错误传播——单条失败不影响同批其他条
# ========================================================================


class TestErrorPropagation(unittest.TestCase):
    def setUp(self):
        self.lib = FakeUMMLib()
        self.desc = self.lib.alloc(PAGE)
        self.engine = ConcurrentIOEngine(self.lib, num_workers=4)
        self.addCleanup(self.engine.close)
        self.payload = _pattern(9, PAGE)
        self.lib.write(self.desc, 0, self.payload)

    def test_per_request_results_and_first_exception(self):
        good1 = bytearray(PAGE)
        good2 = bytearray(PAGE)
        reqs = [
            IORequest(_addr(self.desc, 0, PAGE), memoryview(good1)),                    # 0: OK
            IORequest(_addr(self.desc, PAGE - 16, 32), memoryview(bytearray(32))),      # 1: 越界
            IORequest(_addr(self.desc, 0, PAGE), memoryview(good2)),                    # 2: OK
            IORequest(_addr(self.desc, 0, 16), memoryview(b"x" * 16)),                  # 3: 只读 buffer
            IORequest(_addr(self.desc, 0, PAGE), memoryview(bytearray(16))),            # 4: buffer 不足
        ]
        h = self.engine.submit_read(reqs)
        res = h.results()

        self.assertEqual(res[0], PAGE)
        self.assertIsInstance(res[1], RuntimeError, "越界读应失败")
        self.assertEqual(res[2], PAGE)
        self.assertIsInstance(res[3], ValueError, "只读 buffer 应 ValueError")
        self.assertIsInstance(res[4], ValueError, "buffer 不足应 ValueError")

        # result() 抛首个异常
        with self.assertRaises(RuntimeError):
            h.result()
        self.assertIs(h.exception(), res[1])

        # 失败不影响同批其他条：好请求数据正确
        self.assertEqual(bytes(good1), self.payload)
        self.assertEqual(bytes(good2), self.payload)

        stats = self.engine.stats()
        self.assertEqual(stats["completed"], 2)
        self.assertEqual(stats["failed"], 3)

    def test_write_error_isolation(self):
        desc2 = self.lib.alloc(PAGE)
        data = _pattern(10, PAGE)
        reqs = [
            IORequest(_addr(desc2, 0, PAGE), data),                       # OK
            IORequest(_addr(desc2, PAGE - 8, PAGE), data),                # 越界
            IORequest(_addr(ChunkDescriptor(999999, 0, PAGE), 0, PAGE), data),  # chunk 不存在
        ]
        h = self.engine.submit_write(reqs)
        res = h.results()
        self.assertEqual(res[0], PAGE)
        self.assertIsInstance(res[1], RuntimeError)
        self.assertIsInstance(res[2], RuntimeError)
        # 成功的那条确实写入
        self.assertEqual(self.lib.read(desc2, 0, PAGE), data)

    def test_negative_offset_length_rejected(self):
        reqs = [
            IORequest(IOAddress(self.desc.chunk_id, self.desc.base_gpa, -1, 16), bytearray(16)),
            IORequest(IOAddress(self.desc.chunk_id, self.desc.base_gpa, 0, -8), bytearray(16)),
        ]
        res = self.engine.submit_read(reqs).results()
        self.assertIsInstance(res[0], ValueError)
        self.assertIsInstance(res[1], ValueError)

    def test_sync_batch_raises_first_exception(self):
        reqs = [
            IORequest(_addr(self.desc, 0, PAGE), bytearray(PAGE)),
            IORequest(_addr(self.desc, PAGE, PAGE), bytearray(PAGE)),  # 越界
        ]
        with self.assertRaises(RuntimeError):
            self.engine.read_batch(reqs)


# ========================================================================
# 测试点 7：背压——max_inflight=1 时并发 submit 不超限
# ========================================================================


class TestBackpressure(unittest.TestCase):
    def test_max_inflight_one(self):
        lib = FakeUMMLib(io_delay=0.01)
        n_threads = 4
        reqs_per_thread = 4
        descs = [lib.alloc(PAGE) for _ in range(n_threads)]

        with ConcurrentIOEngine(lib, num_workers=4, max_inflight=1) as engine:
            def worker(i):
                for _ in range(reqs_per_thread):
                    engine.submit_read(
                        [IORequest(_addr(descs[i], 0, PAGE), bytearray(PAGE))]
                    ).result()

            threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_threads)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()

            # 设备侧观察到的并发 I/O 峰值不得超过 max_inflight=1
            self.assertLessEqual(lib.fake.max_device_inflight, 1)
            stats = engine.stats()
            total = n_threads * reqs_per_thread
            self.assertEqual(stats["submitted"], total)
            self.assertEqual(stats["completed"], total)
            self.assertEqual(stats["inflight"], 0)

    def test_submit_blocks_until_slot_free(self):
        """背压下 submit 会阻塞，直到在途请求完成释放许可。"""
        lib = FakeUMMLib(io_delay=0.05)
        desc = lib.alloc(PAGE)
        with ConcurrentIOEngine(lib, num_workers=2, max_inflight=1) as engine:
            engine.submit_read([IORequest(_addr(desc, 0, PAGE), bytearray(PAGE))])
            t0 = time.monotonic()
            h2 = engine.submit_read([IORequest(_addr(desc, 0, PAGE), bytearray(PAGE))])
            elapsed = time.monotonic() - t0
            # 第二次 submit 必须等到第一次的 I/O（约 50ms）完成
            self.assertGreaterEqual(elapsed, 0.03)
            self.assertEqual(h2.result(), [PAGE])
            self.assertLessEqual(lib.fake.max_device_inflight, 1)


# ========================================================================
# 测试点 8：buffer 前缀语义——buffer 大于 length 时只读写前 length 字节
# ========================================================================


class TestBufferPrefixSemantics(unittest.TestCase):
    def test_prefix_read_write(self):
        lib = FakeUMMLib()
        desc = lib.alloc(PAGE * 2)
        head = _pattern(11, PAGE)

        with ConcurrentIOEngine(lib, num_workers=4) as engine:
            # 写：buffer 8KB，length 4KB → 只写前 4KB
            big_w = bytearray(PAGE * 2)
            big_w[:PAGE] = head
            big_w[PAGE:] = b"\xFF" * PAGE   # 尾部不应被写出
            res = engine.write_batch([IORequest(_addr(desc, 0, PAGE), memoryview(big_w))])
            self.assertEqual(res, [PAGE])
            raw = lib.read(desc, 0, PAGE * 2)
            self.assertEqual(raw[:PAGE], head)
            self.assertEqual(raw[PAGE:], b"\x00" * PAGE, "超出 length 的字节不应被写")

            # 读：buffer 8KB，length 4KB → 前 4KB 填充，尾部保持不动
            big_r = bytearray(b"\xEE" * PAGE * 2)
            res = engine.read_batch([IORequest(_addr(desc, 0, PAGE), memoryview(big_r))])
            self.assertEqual(res, [PAGE])
            self.assertEqual(bytes(big_r[:PAGE]), head)
            self.assertEqual(big_r[PAGE:], b"\xEE" * PAGE, "超出 length 的 buffer 前缀外字节不应被动")

            # 等长 buffer：整个 buffer 都被使用
            exact = bytearray(PAGE)
            self.assertEqual(engine.read_batch([IORequest(_addr(desc, 0, PAGE), exact)]), [PAGE])
            self.assertEqual(bytes(exact), head)


# ========================================================================
# 补充：句柄组合 / submit_mixed / from_block / 上下文管理器与关闭语义
# ========================================================================


class TestEngineApi(unittest.TestCase):
    def test_wait_any_returns_first_done(self):
        lib = FakeUMMLib(io_delay=0.01)
        desc = lib.alloc(PAGE)
        lib.write(desc, 0, _pattern(12, PAGE))
        with ConcurrentIOEngine(lib, num_workers=2) as engine:
            h_fast = engine.submit_read([IORequest(_addr(desc, 0, PAGE), bytearray(PAGE))])
            h_slow = engine.submit_read(
                [IORequest(_addr(desc, 0, PAGE), bytearray(PAGE)) for _ in range(8)]
            )
            first = ConcurrentIOEngine.wait_any([h_fast, h_slow], timeout=10.0)
            self.assertIs(first, h_fast)
            ConcurrentIOEngine.wait_all([h_fast, h_slow])
            # 全部完成后再 wait_any：返回首个已完成的
            self.assertIs(ConcurrentIOEngine.wait_any([h_slow, h_fast], timeout=1.0), h_slow)

    def test_wait_any_timeout_and_empty(self):
        lib = FakeUMMLib(io_delay=0.2)
        desc = lib.alloc(PAGE)
        with ConcurrentIOEngine(lib, num_workers=1) as engine:
            with self.assertRaises(ValueError):
                ConcurrentIOEngine.wait_any([], timeout=0.01)
            h = engine.submit_read([IORequest(_addr(desc, 0, 16), bytearray(16))])
            with self.assertRaises(TimeoutError):
                ConcurrentIOEngine.wait_any([h], timeout=0.02)
            with self.assertRaises(TimeoutError):
                ConcurrentIOEngine.wait_all([h], timeout=0.02)
            h.result(timeout=2.0)

    def test_submit_mixed_order(self):
        lib = FakeUMMLib()
        desc = lib.alloc(PAGE)
        data = _pattern(13, PAGE)
        with ConcurrentIOEngine(lib, num_workers=4) as engine:
            w = [IORequest(_addr(desc, 0, PAGE), data, opaque="w")]
            h = engine.submit_mixed([], w)
            self.assertEqual(h.result(), [PAGE])
            rbuf = bytearray(PAGE)
            r = [IORequest(_addr(desc, 0, PAGE), rbuf, opaque="r")]
            w2 = [IORequest(_addr(desc, PAGE // 2, 128), b"\xAB" * 128, opaque="w2")]
            h = engine.submit_mixed(r, w2)
            res = h.results()
            self.assertEqual(res[0], PAGE)   # reads 在前
            self.assertEqual(res[1], 128)    # writes 在后
            self.assertEqual([req.opaque for req in h.requests], ["r", "w2"])
            self.assertEqual(bytes(rbuf), data)

    def test_from_block(self):
        lib = FakeUMMLib()
        desc = lib.alloc(PAGE * 2)
        block = Block(chunk_id=desc.chunk_id, offset=128, size=PAGE, gpa=desc.base_gpa,
                      chunk_size=PAGE * 2)
        data = _pattern(14, 256)
        with ConcurrentIOEngine(lib) as engine:
            # 默认：整个 block 区间
            addr = IOAddress.from_block(block)
            self.assertEqual((addr.chunk_id, addr.base_gpa, addr.offset, addr.length),
                             (desc.chunk_id, desc.base_gpa, 128, PAGE))
            # 指定 block 内偏移与长度
            addr = IOAddress.from_block(block, offset=64, length=256)
            self.assertEqual(engine.write_batch([IORequest(addr, data)]), [256])
            buf = bytearray(256)
            self.assertEqual(engine.read_batch([IORequest(addr, buf)]), [256])
            self.assertEqual(bytes(buf), data)
        # 落点 = block.offset + offset = 192
        self.assertEqual(lib.read(desc, 192, 256), data)

    def test_close_and_submit_after_close(self):
        lib = FakeUMMLib()
        desc = lib.alloc(PAGE)
        engine = ConcurrentIOEngine(lib)
        engine.close()
        with self.assertRaises(RuntimeError):
            engine.submit_read([IORequest(_addr(desc, 0, 16), bytearray(16))])
        engine.close()  # 幂等

    def test_empty_batch(self):
        lib = FakeUMMLib()
        with ConcurrentIOEngine(lib) as engine:
            h = engine.submit_read([])
            self.assertTrue(h.done())
            self.assertEqual(h.result(), [])
            fired = threading.Event()
            h.add_done_callback(lambda hh: fired.set())
            self.assertTrue(fired.is_set())
            self.assertEqual(engine.read_batch([]), [])

    def test_constructor_validation(self):
        lib = FakeUMMLib()
        with self.assertRaises(ValueError):
            ConcurrentIOEngine(lib, num_workers=0)
        with self.assertRaises(ValueError):
            ConcurrentIOEngine(lib, num_queues=0)
        with self.assertRaises(ValueError):
            ConcurrentIOEngine(lib, max_inflight=0)


if __name__ == "__main__":
    unittest.main()
