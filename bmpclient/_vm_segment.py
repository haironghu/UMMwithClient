# -*- coding: utf-8 -*-
"""
bmpclient/_vm_segment.py — VirtualMedia 内部模块：盘内段（super page）
分配器与聚合写缓冲。不作为公开接口使用。

段只是**写侧**分配与回收的管理单位，不参与读路径寻址：
segment_id = offset // sp_bytes。读路径一律用 extent 内绝对偏移。

设计文档：bmpclient/docs/07_VirtualMedia合并设计_稀疏KV专用介质.md §3。
"""

import threading
from typing import List, Optional

from bmpclient.umm_client import ChunkDescriptor, UMMLib


class SegmentFullError(RuntimeError):
    """该盘 extent 的所有段已分配完且无归零段可回收。"""
    pass


def check_segment_geometry(sp_bytes: int, unit_size: int) -> None:
    """校验段大小约束：正数、4KB 整数倍、能装整数个单元。"""
    if sp_bytes <= 0:
        raise ValueError(f"sp_bytes 必须为正数: {sp_bytes}")
    if sp_bytes % 4096 != 0:
        raise ValueError(f"sp_bytes 必须是 4KB 整数倍: {sp_bytes}")
    if unit_size <= 0 or unit_size % 4096 != 0:
        raise ValueError(f"unit_size 必须是 4KB 整数倍: {unit_size}")
    if sp_bytes % unit_size != 0:
        raise ValueError(
            f"sp_bytes ({sp_bytes}) 必须能整除 unit_size ({unit_size})"
        )


class DeviceSegmentManager:
    """
    单块 SSD extent 的段级写侧管理。

    :param lib: UMMLib（或 FakeUMMLib）
    :param desc: 本盘 extent 的 ChunkDescriptor（alloc_on_device 分配）
    :param device_idx: 设备索引（仅日志/报错用）
    :param sp_bytes: 段大小（正数、4KB 整数倍、unit_size 整数倍）
    :param unit_size: 排布单元定长（4KB 对齐）
    """

    def __init__(
        self,
        lib: UMMLib,
        desc: ChunkDescriptor,
        device_idx: int,
        sp_bytes: int,
        unit_size: int,
    ):
        check_segment_geometry(sp_bytes, unit_size)
        if desc.user_size % sp_bytes != 0:
            raise ValueError(
                f"extent 大小 ({desc.user_size}) 必须是段大小 ({sp_bytes}) 的整数倍"
            )
        self._lib = lib
        self._desc = desc
        self._device_idx = device_idx
        self._sp_bytes = sp_bytes
        self._unit_size = unit_size
        self._slots_per_seg = sp_bytes // unit_size
        self._n_seg = desc.user_size // sp_bytes

        self._lock = threading.Lock()
        self._free = list(range(self._n_seg - 1, -1, -1))  # 空闲段栈（pop 小号）
        self._refcount = [0] * self._n_seg                 # 段内有效单元数
        # 当前聚合写缓冲（未下盘单元的数据所在）
        self._buf: Optional[bytearray] = None
        self._buf_seg = -1
        self._buf_fill = 0                                 # 已填充槽位数

    # ---- 属性 ----

    @property
    def desc(self) -> ChunkDescriptor:
        """本盘 extent 的 ChunkDescriptor（读路径构造 IOAddress 用）。"""
        return self._desc

    @property
    def sp_bytes(self) -> int:
        return self._sp_bytes

    @property
    def num_free_segments(self) -> int:
        with self._lock:
            return len(self._free)

    @property
    def buffered_units(self) -> int:
        """当前段缓冲中已聚合、未下盘的单元数。"""
        with self._lock:
            return self._buf_fill

    def segment_of(self, offset: int) -> int:
        """由 extent 内偏移推出所属段号。"""
        return offset // self._sp_bytes

    # ---- 写路径 ----

    def append(self, data: bytes) -> int:
        """
        把一个单元追加进当前段缓冲，返回其在 extent 内的字节偏移。
        当前段写满时先将其整段下盘（一次连续 IO）再开新段。
        段耗尽抛 SegmentFullError。
        """
        if len(data) != self._unit_size:
            raise ValueError(
                f"单元长度 {len(data)} != unit_size {self._unit_size}"
            )
        with self._lock:
            if self._buf is None:
                self._start_segment()
            if self._buf_fill == self._slots_per_seg:
                # 段满：先把整段下盘再开新段；本次写入始终落在缓冲内，
                # 保证返回后该偏移处于"缓冲中"状态
                self._flush_locked()
                self._start_segment()
            seg = self._buf_seg
            offset = seg * self._sp_bytes + self._buf_fill * self._unit_size
            start = self._buf_fill * self._unit_size
            self._buf[start:start + self._unit_size] = data
            self._buf_fill += 1
            self._refcount[seg] += 1
            return offset

    def flush(self) -> None:
        """强制把当前半满段下盘（半满段也按整段长度下发，未用槽位为零页）。"""
        with self._lock:
            if self._buf is not None and self._buf_fill > 0:
                self._flush_locked()

    def _start_segment(self) -> None:
        if not self._free:
            raise SegmentFullError(
                f"SSD{self._device_idx} 段已耗尽（共 {self._n_seg} 段）"
            )
        self._buf_seg = self._free.pop()
        self._buf = bytearray(self._sp_bytes)
        self._buf_fill = 0

    def _flush_locked(self) -> None:
        """当前段作为一次连续大 IO 写入 extent 对应偏移区间（锁内调用）。"""
        seg = self._buf_seg
        self._lib.write_from(self._desc, seg * self._sp_bytes, self._buf)
        self._buf = None
        self._buf_seg = -1
        self._buf_fill = 0
        if self._refcount[seg] == 0:
            # 段内单元已在下盘前全部 release：直接回收，不落空闲泄漏
            self._free.append(seg)

    # ---- 读路径辅助（未下盘单元从缓冲直接读）----

    def is_buffered(self, offset: int) -> bool:
        """该偏移是否仍在当前聚合缓冲（未下盘）。"""
        with self._lock:
            return self._buf is not None and self.segment_of(offset) == self._buf_seg

    def read_buffer(self, offset: int, size: int) -> Optional[bytes]:
        """
        从当前段缓冲读未下盘单元；offset 不在当前缓冲内返回 None
        （调用方应回退到盘读——可能刚被 flush 下盘）。
        """
        with self._lock:
            if self._buf is None or self.segment_of(offset) != self._buf_seg:
                return None
            local = offset - self._buf_seg * self._sp_bytes
            return bytes(self._buf[local:local + size])

    # ---- 回收 ----

    def release(self, offset: int) -> None:
        """释放一个单元：递减所属段引用计数，归零段回空闲池。"""
        with self._lock:
            seg = self.segment_of(offset)
            if seg == self._buf_seg:
                # 当前填充中的段不回收（仍在写入）
                self._refcount[seg] -= 1
                return
            if self._refcount[seg] <= 0:
                raise RuntimeError(
                    f"SSD{self._device_idx} 段 {seg} 引用计数异常（重复 release?）"
                )
            self._refcount[seg] -= 1
            if self._refcount[seg] == 0:
                self._free.append(seg)
