# -*- coding: utf-8 -*-
"""
bmpclient/testing.py — 测试工具：内存版 UMMLib（FakeUMMLib）。

FakeUMMLib 复用 UMMLib 的全部方法（含零拷贝 read_into/write_from），
底层 C 入口由 _FakeCLib 在 ctypes 指针层面模拟：
接收真实 ctypes 指针、直接读写内存 dict、线程安全。

用途：
- 单元测试（tests/test_concurrent_io.py）替代真实 libumm.so；
- 慢设备模拟（scripts/demo_e2e_servers.py 场景 C）：io_delay 注入设备延迟，
  验证并发引擎在「单 IO 存在真实等待」场景下的加速效果；
- 任何需要在无 UMM 环境下演练 client 代码的场景。
"""

import ctypes
import threading
import time

from bmpclient.umm_client import ChunkDescriptor, UMMLib

_DESC_PTR = ctypes.POINTER(ChunkDescriptor)


class _FakeCLib:
    """
    模拟 libumm.so 的 C 入口：接收 ctypes 指针，直接读写内存 dict。

    - 线程安全：chunk 数据的读取快照 / 写入落盘均在锁内完成；
    - io_delay 可注入设备延迟（在锁外 sleep，允许并发，用于异步/保序/背压测试）；
    - 记录设备侧并发度与最近收到的数据指针（零拷贝断言用）。
    """

    def __init__(self, io_delay: float = 0.0):
        self._lock = threading.Lock()
        self._chunks = {}          # chunk_id -> bytearray
        self._next_chunk_id = 1
        self.io_delay = io_delay
        self.device_inflight = 0       # 当前设备侧并发 I/O 数
        self.max_device_inflight = 0   # 历史峰值
        self.last_read_ptr = None      # 最近一次 umm_read 收到的数据指针
        self.last_write_ptr = None     # 最近一次 umm_write 收到的数据指针

    # ---- chunk 生命周期 ----

    def umm_alloc(self, size, desc_ptr):
        desc = ctypes.cast(desc_ptr, _DESC_PTR).contents
        with self._lock:
            cid = self._next_chunk_id
            self._next_chunk_id += 1
            self._chunks[cid] = bytearray(size)
            desc.chunk_id = cid
            desc.base_gpa = 0x100000 + cid * 0x10000
            desc.user_size = size
        return 0

    def umm_free(self, desc_ptr):
        desc = ctypes.cast(desc_ptr, _DESC_PTR).contents
        with self._lock:
            if desc.chunk_id not in self._chunks:
                return 1
            del self._chunks[desc.chunk_id]
        return 0

    # ---- 读写（签名与真实 umm_read/umm_write 一致）----

    def umm_read(self, desc_ptr, offset, size, buf):
        desc = ctypes.cast(desc_ptr, _DESC_PTR).contents
        with self._lock:
            data = self._chunks.get(desc.chunk_id)
            if data is None:
                return 1
            if offset + size > len(data):
                return 2
            snapshot = bytes(data[offset:offset + size])
            self.last_read_ptr = self._ptr_value(buf)
        self._io_enter()
        try:
            if self.io_delay:
                time.sleep(self.io_delay)
            if size:
                ctypes.memmove(buf, snapshot, size)
        finally:
            self._io_exit()
        return 0

    def umm_write(self, desc_ptr, offset, size, buf):
        desc = ctypes.cast(desc_ptr, _DESC_PTR).contents
        with self._lock:
            data = self._chunks.get(desc.chunk_id)
            if data is None:
                return 1
            if offset + size > len(data):
                return 2
            self.last_write_ptr = self._ptr_value(buf)
        # 先从调用方 buffer 取数（模拟设备 DMA），再在锁内落盘
        payload = ctypes.string_at(buf, size) if size else b""
        self._io_enter()
        try:
            if self.io_delay:
                time.sleep(self.io_delay)
            with self._lock:
                if size:
                    data[offset:offset + size] = payload
        finally:
            self._io_exit()
        return 0

    def umm_error_string(self, rc):
        return {1: b"chunk not found", 2: b"out of range"}.get(rc, b"fake error")

    # ---- 内部 ----

    @staticmethod
    def _ptr_value(buf):
        return buf.value if isinstance(buf, ctypes.c_void_p) else int(buf)

    def _io_enter(self):
        with self._lock:
            self.device_inflight += 1
            if self.device_inflight > self.max_device_inflight:
                self.max_device_inflight = self.device_inflight

    def _io_exit(self):
        with self._lock:
            self.device_inflight -= 1

    def reset_stats(self):
        """清空并发峰值等观测计数（复用同一实例跑多组实验时用）。"""
        with self._lock:
            self.device_inflight = 0
            self.max_device_inflight = 0


class FakeUMMLib(UMMLib):
    """不加载 libumm.so 的 UMMLib：内存 dict 模拟 chunk 存储（threading 安全）。"""

    def __init__(self, io_delay: float = 0.0):
        # 绕过 UMMLib.__init__ 的 CDLL 加载，直接替换底层 C 库句柄；
        # read/write/read_into/write_from 等方法原样复用。
        self._lib = _FakeCLib(io_delay)

    @property
    def fake(self) -> _FakeCLib:
        return self._lib
