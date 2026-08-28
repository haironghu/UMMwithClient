# -*- coding: utf-8 -*-
"""
bmpclient/sparse_kv/store.py — SparseKVStore：稀疏 KV cache 框架对接层。

职责（docs/07 §5）：
- 对接推理框架：KVBlockRef 逐 token 展开、请求生命周期；
- slot_table 元数据：(layer, token) -> extent 内偏移（flags 仅 VALID）；
- **地址规划 plan()**：把 (layer, topk token_indices) 翻译成存储侧地址
  映射表（descriptor buffer），供 GPU 直通存储算子在图内发起加载；
  本项目不执行生产读 IO。

descriptor buffer 布局（与 GPU 算子的唯一契约，32B 定长 entry）：

    header: { count : u32, layer_id : u32, reserved : u64 }
    entry[i]: { ssd_id : u32, flags : u32, lba_offset : u64,
                length : u64, dst_offset : u64 }

flags：0=DISK（直通读）；1=HOST_READY（IN_BUF 兜底，CPU 已回填 staging，
算子跳过该条 IO）。buffer 地址固定（初始化预分配）、内容逐 step 覆写，
满足图模式约束。当前实现用 ctypes buffer 占位；生产环境应由框架替换为
pinned host 内存或显存（接口不变，仅换底层 buffer）。

注意 release 安全窗口（docs/07 §5.3 规则 3）：plan 引用的 entry，
其 release 必须延后到该步图执行完成之后。
"""

import ctypes
import struct
import threading
from typing import Dict, List, Optional, Tuple

from bmpclient.sparse_kv.slot_table import (
    FLAG_VALID,
    SlotTable,
    entry_flags,
    entry_offset,
)
from bmpclient.sparse_kv.vllm_adapter import KVBlockRef
from bmpclient.virtual_media import VirtualMedia

# descriptor buffer 二进制布局（小端）
HEADER_STRUCT = struct.Struct("<IIQ")       # count, layer_id, reserved = 16B
ENTRY_STRUCT = struct.Struct("<IIQQQ")      # 32B
HEADER_SIZE = HEADER_STRUCT.size
ENTRY_SIZE = ENTRY_STRUCT.size

FLAG_DISK = 0         # GPU 直通算子从盘读
FLAG_HOST_READY = 1   # CPU 已兜底回填 staging，算子跳过 IO


class PlanView:
    """一次 plan() 的产出：descriptor buffer 的只读视图（地址固定）。"""

    def __init__(self, buf: ctypes.Array, count: int, layer_id: int,
                 max_topk: int, unit_size: int):
        self._buf = buf
        self.count = count
        self.layer_id = layer_id
        self.max_topk = max_topk
        self.unit_size = unit_size

    @property
    def address(self) -> int:
        """buffer 固定地址（图内算子按此读取；逐 step 不变）。"""
        return ctypes.addressof(self._buf)

    @property
    def nbytes(self) -> int:
        return HEADER_SIZE + ENTRY_SIZE * self.max_topk

    def entry(self, i: int) -> Tuple[int, int, int, int, int]:
        """解析第 i 条：(ssd_id, flags, lba_offset, length, dst_offset)。"""
        if not 0 <= i < self.count:
            raise IndexError(f"entry 索引越界: {i} >= count {self.count}")
        return ENTRY_STRUCT.unpack_from(self._buf, HEADER_SIZE + i * ENTRY_SIZE)

    def entries(self) -> List[Tuple[int, int, int, int, int]]:
        return [self.entry(i) for i in range(self.count)]


class SparseKVStore:
    """
    稀疏 KV cache 卸载/地址规划（单请求作用域）。

    :param vm: VirtualMedia 介质层实例
    :param num_layers: 模型层数
    :param max_tokens: 最大上下文 token 数（slot_table 按此预留）
    :param max_topk: 单步 topk 上限（descriptor buffer 按此预留定长 entry）
    """

    def __init__(self, vm: VirtualMedia, num_layers: int, max_tokens: int,
                 max_topk: int = 512):
        if max_topk <= 0:
            raise ValueError("max_topk 必须为正")
        self._vm = vm
        self._num_layers = num_layers
        self._max_tokens = max_tokens
        self._max_topk = max_topk
        self._unit_size = vm.unit_size
        self._table = SlotTable(num_layers, max_tokens)
        self._write_lock = threading.Lock()
        self._cache: Dict[Tuple[int, int], bytes] = {}  # prefetch 读缓存
        self._closed = False

        # 固定地址 descriptor buffer（生产环境替换为 pinned/显存 buffer）
        self._desc_buf = (ctypes.c_char * (HEADER_SIZE + ENTRY_SIZE * max_topk))()
        # HOST_READY 兜底回填的暂存（生产环境即 GPU 直通算子的 staging buffer）
        self._staging = bytearray(max_topk * self._unit_size)

    # ------------------------------------------------------------------
    # 属性
    # ------------------------------------------------------------------

    @property
    def media(self) -> VirtualMedia:
        return self._vm

    @property
    def slot_table(self) -> SlotTable:
        return self._table

    @property
    def staging(self) -> bytearray:
        """兜底回填暂存（生产环境为算子 staging 的 CPU 侧映射）。"""
        return self._staging

    # ------------------------------------------------------------------
    # 卸载（写路径，CPU，不入图）
    # ------------------------------------------------------------------

    def offload(self, blocks: List[KVBlockRef]) -> None:
        """逐 token 展开 → vm.write 按策略分盘聚合 → slot_table 记偏移。"""
        with self._write_lock:
            for blk in blocks:
                self._check_block(blk)
                for i in range(blk.token_count):
                    t = blk.token_start + i
                    unit = blk.buffer[
                        i * self._unit_size:(i + 1) * self._unit_size
                    ]
                    offset = self._vm.write((blk.layer_id, t), unit.tobytes())
                    self._table.set(blk.layer_id, t, offset, FLAG_VALID)

    def _check_block(self, blk: KVBlockRef) -> None:
        if not 0 <= blk.layer_id < self._num_layers:
            raise ValueError(f"layer_id 越界: {blk.layer_id}")
        if blk.token_start < 0 or blk.token_start + blk.token_count > self._max_tokens:
            raise ValueError(
                f"token 范围越界: [{blk.token_start}, "
                f"{blk.token_start + blk.token_count}) / {self._max_tokens}"
            )
        if blk.buffer.nbytes < blk.token_count * self._unit_size:
            raise ValueError(
                f"block buffer 不足: {blk.buffer.nbytes} < "
                f"{blk.token_count} * {self._unit_size}"
            )

    def flush(self) -> None:
        """强制刷所有盘的半满段缓冲（prefill→decode 切换/请求边界调用）。"""
        self._vm.flush()

    # ------------------------------------------------------------------
    # 地址规划（读路径：本项目只输出地址映射，IO 由 GPU 直通算子执行）
    # ------------------------------------------------------------------

    def plan(self, layer_id: int, token_indices: List[int]) -> PlanView:
        """
        把 (layer_id, topk token_indices) 翻译成地址映射表，写入固定地址
        descriptor buffer，返回视图。GPU 直通算子按此在图内发起加载。

        IN_BUF（未下盘）命中的单元由 CPU 直接回填 staging 对应
        dst_offset，entry 标记 HOST_READY，算子跳过该条 IO。
        """
        if len(token_indices) > self._max_topk:
            raise ValueError(
                f"topk 规模 {len(token_indices)} 超过 max_topk {self._max_topk}"
            )
        keys = [(layer_id, t) for t in token_indices]
        devices = self._vm.locate_batch(keys)              # ① 算盘号（零元数据）
        entries = self._table.gather(layer_id, token_indices)  # ② gather 偏移

        for i, (t, e) in enumerate(zip(token_indices, entries)):
            if not entry_flags(e) & FLAG_VALID:
                raise ValueError(f"({layer_id}, {t}) 未卸载或已释放")
            d = devices[i]
            off = entry_offset(e)
            dst = i * self._unit_size
            if self._vm.is_buffered(d, off):
                data = self._vm.read_buffered(d, off)
                if data is not None:
                    # 规则 B：IN_BUF 兜底——CPU 回填 staging，算子跳过
                    self._staging[dst:dst + self._unit_size] = data
                    flags = FLAG_HOST_READY
                else:
                    flags = FLAG_DISK   # 与并发 flush 竞争：已在盘上
            else:
                flags = FLAG_DISK
            ENTRY_STRUCT.pack_into(
                self._desc_buf, HEADER_SIZE + i * ENTRY_SIZE,
                d, flags, self._vm.extent_base(d) + off,
                self._unit_size, dst,
            )
        HEADER_STRUCT.pack_into(self._desc_buf, 0, len(token_indices), layer_id, 0)
        return PlanView(self._desc_buf, len(token_indices), layer_id,
                        self._max_topk, self._unit_size)

    # ------------------------------------------------------------------
    # CPU 兜底读（联调/无直通硬件环境用；生产读路径不走这里）
    # ------------------------------------------------------------------

    def fetch(self, layer_id: int, token_indices: List[int],
              out: List[memoryview]) -> None:
        """= 地址规划 + vm.read_batch 执行，仅验证与降级场景。"""
        if len(out) != len(token_indices):
            raise ValueError("out 与 token_indices 长度不一致")
        keys = [(layer_id, t) for t in token_indices]
        devices = self._vm.locate_batch(keys)
        entries = self._table.gather(layer_id, token_indices)
        items: List[Tuple[int, int]] = []
        real_out: List[memoryview] = []
        for pos, (t, e) in enumerate(zip(token_indices, entries)):
            if not entry_flags(e) & FLAG_VALID:
                raise ValueError(f"({layer_id}, {t}) 未卸载或已释放")
            cached = self._cache.get((layer_id, t))
            if cached is not None:
                out[pos][: self._unit_size] = cached
                continue
            items.append((devices[pos], entry_offset(e)))
            real_out.append(out[pos])
        if items:
            self._vm.read_batch(items, real_out)

    def prefetch(self, layer_id: int, token_indices: List[int]) -> None:
        """层间流水预取：读入内部缓存，后续 fetch 命中直接返回。"""
        pending = [t for t in token_indices
                   if (layer_id, t) not in self._cache]
        if not pending:
            return
        bufs = [memoryview(bytearray(self._unit_size)) for _ in pending]
        self.fetch(layer_id, pending, bufs)
        for t, b in zip(pending, bufs):
            self._cache[(layer_id, t)] = b.tobytes()

    # ------------------------------------------------------------------
    # 回收与生命周期
    # ------------------------------------------------------------------

    def release(self, layer_id: int, token_indices: List[int]) -> None:
        """批量失效：entry 置 invalid + 递减所属段引用计数（归零段回收）。

        注意（docs/07 §5.3 规则 3）：若这些 token 刚被 plan 引用，必须等
        该步图执行完成后再 release（段复用即地址复用）。
        """
        for t in token_indices:
            e = self._table.get(layer_id, t)
            if not entry_flags(e) & FLAG_VALID:
                continue
            d = self._vm.locate((layer_id, t))
            self._table.invalidate(layer_id, t)
            self._cache.pop((layer_id, t), None)
            self._vm.release(d, entry_offset(e))

    def close(self) -> None:
        """关闭介质层（未 flush 的半满段数据随之丢失）。"""
        if self._closed:
            return
        self._closed = True
        self._vm.close()

    def __enter__(self) -> "SparseKVStore":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> bool:
        self.close()
        return False
