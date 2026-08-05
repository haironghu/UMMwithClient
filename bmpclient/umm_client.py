"""
bmpclient/umm_client.py — ctypes 绑定，封装 UMM C API。

加载 UMM 的 libumm.so，提供 Pythonic 的 Chunk 分配 / 释放 / 读写接口。
"""

import ctypes
import os
import sys

# ========================================================================
# C structure definitions (must match include/umm.h)
# ========================================================================


class ChunkDescriptor(ctypes.Structure):
    _fields_ = [
        ("chunk_id", ctypes.c_uint64),
        ("base_gpa", ctypes.c_uint64),
        ("user_size", ctypes.c_uint64),
    ]

    def __repr__(self):
        return f"ChunkDescriptor(chunk_id={self.chunk_id}, base_gpa=0x{self.base_gpa:016x}, user_size={self.user_size})"


class ChunkMetadata(ctypes.Structure):
    _fields_ = [
        ("chunk_id", ctypes.c_uint64),
        ("name", ctypes.c_char * 64),
        ("gpa", ctypes.c_uint64),
        ("size", ctypes.c_uint64),
        ("primary_tier", ctypes.c_uint8),
        ("has_ssd_copy", ctypes.c_uint8),
    ]


class SsdDeviceConfig(ctypes.Structure):
    _fields_ = [
        ("path", ctypes.c_char * 256),
        ("size", ctypes.c_uint64),
    ]


class StorageResource(ctypes.Structure):
    _fields_ = [
        ("tier", ctypes.c_uint8),
        ("device_path", ctypes.c_char * 256),
        ("capacity", ctypes.c_uint64),
        ("base_offset", ctypes.c_uint64),
        ("online", ctypes.c_int),
    ]


UMM_MAX_SSD_DEVICES = 16
UMM_MAX_TOPOLOGY_RESOURCES = 4 + UMM_MAX_SSD_DEVICES


class StorageTopology(ctypes.Structure):
    _fields_ = [
        ("node_id", ctypes.c_uint8),
        ("num_resources", ctypes.c_uint32),
        ("resources", StorageResource * UMM_MAX_TOPOLOGY_RESOURCES),
    ]


class UMMConfig(ctypes.Structure):
    """必须与 umm/include/umm.h 的 UMMConfig 布局逐字段一致。

    注意：umm_init 内部 memcpy(&g_state.config, cfg, sizeof(UMMConfig))，
    ctypes 镜像缺字段 = C 侧越界读 Python 堆（存量 bug，Phase 1 修复）。
    改动 C 侧 UMMConfig 后必须同步本镜像并核对 sizeof。"""
    _fields_ = [
        ("transport", ctypes.c_char * 16),
        ("consistency_model", ctypes.c_char * 16),
        ("memory_size", ctypes.c_uint64),
        ("meta_server_addr", ctypes.c_char * 256),
        ("mem_server_addr", ctypes.c_char * 256),
        ("cxl_device", ctypes.c_char * 256),
        ("ssd_device", ctypes.c_char * 256),
        ("my_node_id", ctypes.c_uint8),
        ("ssd_devices", SsdDeviceConfig * 16),
        ("num_ssd_devices", ctypes.c_uint32),
        ("listen_port", ctypes.c_uint16),
        ("base_gpa", ctypes.c_uint64),
        # ---- NDS RPC server 托管配置（仅 umms 使用；此前镜像缺失！） ----
        ("nds_rpc_server_enable", ctypes.c_int),
        ("nds_rpc_server_ctrl", ctypes.c_char * 256),
        ("nds_rpc_server_ns", ctypes.c_uint32),
        ("nds_rpc_server_qd", ctypes.c_uint32),
        ("nds_rpc_server_socket", ctypes.c_char * 256),
        ("nds_rpc_server_keep_alive", ctypes.c_int),
        # ---- Phase 1: 多节点远程数据面 ----
        ("peer_nodes", ctypes.c_char * 1024),
        ("rpc_token", ctypes.c_char * 64),
        ("allow_cidrs", ctypes.c_char * 512),
        ("ssd_owner_node", ctypes.c_uint8),
        # Phase 2 混合池：1 = 本地内存数据面注册 DRAM tier（默认 0=CXL 旧行为）
        ("local_mem_as_dram", ctypes.c_uint8),
        ("_reserved_phase1", ctypes.c_uint8 * 2),
        ("data_max_io", ctypes.c_uint32),
    ]


# ssd_owner_node 哨兵值（match include/umm.h UMM_NODE_UNKNOWN）
UMM_NODE_UNKNOWN = 0xFF


# Tier IDs (match include/umm.h)
UMM_TIER_DRAM = 0
UMM_TIER_CXL = 1
UMM_TIER_SSD = 2
UMM_TIER_RESV = 3

UMM_OK = 0


def find_libumm_so() -> str:
    """
    按优先级查找 libumm.so：
      1. UMM_BUILD_DIR 环境变量
      2. ../UMM/build/libumm.so
      3. ../../UMM/build/libumm.so
    """
    candidates = []
    env = os.environ.get("UMM_BUILD_DIR")
    if env:
        candidates.append(os.path.join(env, "libumm.so"))

    script_dir = os.path.dirname(os.path.abspath(__file__))
    candidates.append(os.path.join(script_dir, "..", "UMM", "build", "libumm.so"))
    candidates.append(os.path.join(script_dir, "..", "..", "UMM", "build", "libumm.so"))

    for path in candidates:
        abs_path = os.path.abspath(path)
        if os.path.isfile(abs_path):
            return abs_path

    raise FileNotFoundError(
        f"libumm.so not found. Searched: {candidates}\n"
        "Please build UMM with: cd UMM && make shared"
    )


class UMMLib:
    """封装 libumm.so 的 ctypes 调用。"""

    def __init__(self, so_path: str = None):
        if so_path is None:
            so_path = find_libumm_so()
        self._lib = ctypes.CDLL(so_path)
        self._setup_signatures()

    def _setup_signatures(self):
        lib = self._lib

        lib.umm_init.argtypes = [ctypes.POINTER(UMMConfig)]
        lib.umm_init.restype = ctypes.c_int

        lib.umm_deinit.argtypes = []
        lib.umm_deinit.restype = None

        lib.umm_alloc.argtypes = [ctypes.c_uint64, ctypes.POINTER(ChunkDescriptor)]
        lib.umm_alloc.restype = ctypes.c_int

        lib.umm_alloc_tiered.argtypes = [
            ctypes.c_uint64,
            ctypes.c_uint8,
            ctypes.POINTER(ChunkDescriptor),
        ]
        lib.umm_alloc_tiered.restype = ctypes.c_int

        lib.umm_register_storage_tier.argtypes = [
            ctypes.c_uint8,
            ctypes.c_char_p,
            ctypes.c_uint64,
        ]
        lib.umm_register_storage_tier.restype = ctypes.c_int

        # umm_alloc_on_device 是较新版本 libumm 才导出的符号；
        # 对缺失该符号的旧库做可选绑定（调用时才报错，不影响其他 API）
        self._fn_alloc_on_device = getattr(lib, "umm_alloc_on_device", None)
        if self._fn_alloc_on_device is not None:
            self._fn_alloc_on_device.argtypes = [
                ctypes.c_uint64,
                ctypes.c_uint8,
                ctypes.c_uint32,
                ctypes.POINTER(ChunkDescriptor),
            ]
            self._fn_alloc_on_device.restype = ctypes.c_int

        lib.umm_free.argtypes = [ctypes.POINTER(ChunkDescriptor)]
        lib.umm_free.restype = ctypes.c_int

        lib.umm_read.argtypes = [
            ctypes.POINTER(ChunkDescriptor),
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_void_p,
        ]
        lib.umm_read.restype = ctypes.c_int

        lib.umm_write.argtypes = [
            ctypes.POINTER(ChunkDescriptor),
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_void_p,
        ]
        lib.umm_write.restype = ctypes.c_int

        lib.umm_lookup_chunk.argtypes = [ctypes.c_char_p, ctypes.POINTER(ChunkMetadata)]
        lib.umm_lookup_chunk.restype = ctypes.c_int

        # umm_fence / umm_invalidate：共享盘读共享场景的落盘与刷盘。
        # umm_invalidate 是新符号，可选绑定（旧库返回 None 由调用方报错）。
        lib.umm_fence.argtypes = []
        lib.umm_fence.restype = None
        self._fn_invalidate = getattr(lib, "umm_invalidate", None)
        if self._fn_invalidate is not None:
            self._fn_invalidate.argtypes = [
                ctypes.POINTER(ChunkDescriptor),
                ctypes.c_uint64,
                ctypes.c_uint64,
            ]
            self._fn_invalidate.restype = ctypes.c_int

        lib.umm_error_string.argtypes = [ctypes.c_int]
        lib.umm_error_string.restype = ctypes.c_char_p

        lib.umm_tier_name.argtypes = [ctypes.c_uint8]
        lib.umm_tier_name.restype = ctypes.c_char_p

        # umm_get_topology 同样是较新版本才导出的符号，可选绑定
        self._fn_get_topology = getattr(lib, "umm_get_topology", None)
        if self._fn_get_topology is not None:
            self._fn_get_topology.argtypes = [ctypes.POINTER(StorageTopology)]
            self._fn_get_topology.restype = ctypes.c_int

    # ------------------------------------------------------------------
    # Public helpers
    # ------------------------------------------------------------------

    def init(self, cfg: UMMConfig) -> int:
        return self._lib.umm_init(ctypes.byref(cfg))

    def deinit(self) -> None:
        self._lib.umm_deinit()

    def alloc(self, size: int) -> ChunkDescriptor:
        desc = ChunkDescriptor()
        rc = self._lib.umm_alloc(size, ctypes.byref(desc))
        if rc != UMM_OK:
            raise RuntimeError(
                f"umm_alloc({size}) failed: rc={rc} ({self.errstr(rc)})"
            )
        return desc

    def alloc_tiered(self, size: int, tier: int) -> ChunkDescriptor:
        desc = ChunkDescriptor()
        rc = self._lib.umm_alloc_tiered(size, tier, ctypes.byref(desc))
        if rc != UMM_OK:
            raise RuntimeError(
                f"umm_alloc_tiered({size}, tier={tier}) failed: rc={rc} ({self.errstr(rc)})"
            )
        return desc

    def register_storage_tier(self, tier: int, device_path: str,
                              capacity: int) -> None:
        """注册 tier 存储设备（direct: 本地池; rpc: 上报 ummD + 本地数据面）。

        RPC 模式下应在 init 之后、任何数据 I/O 之前调用（tier router
        会整体重建）。libnvm 设备需在进程环境中先设好
        UMM_LIBNVM_PATH / LD_LIBRARY_PATH。
        """
        rc = self._lib.umm_register_storage_tier(
            tier, device_path.encode("utf-8"), capacity
        )
        if rc != UMM_OK:
            raise RuntimeError(
                f"umm_register_storage_tier(tier={tier}, dev={device_path}) "
                f"failed: rc={rc} ({self.errstr(rc)})"
            )

    def alloc_on_device(self, size: int, tier: int, device_idx: int) -> ChunkDescriptor:
        if self._fn_alloc_on_device is None:
            raise RuntimeError(
                "当前 libumm.so 未导出 umm_alloc_on_device（需较新版本 UMM）"
            )
        desc = ChunkDescriptor()
        rc = self._fn_alloc_on_device(size, tier, device_idx, ctypes.byref(desc))
        if rc != UMM_OK:
            raise RuntimeError(
                f"umm_alloc_on_device({size}, tier={tier}, device={device_idx}) "
                f"failed: rc={rc} ({self.errstr(rc)})"
            )
        return desc

    def get_topology(self) -> StorageTopology:
        if self._fn_get_topology is None:
            raise RuntimeError(
                "当前 libumm.so 未导出 umm_get_topology（需较新版本 UMM）"
            )
        topo = StorageTopology()
        rc = self._fn_get_topology(ctypes.byref(topo))
        if rc != UMM_OK:
            raise RuntimeError(
                f"umm_get_topology failed: rc={rc} ({self.errstr(rc)})"
            )
        return topo

    def free(self, desc: ChunkDescriptor) -> None:
        rc = self._lib.umm_free(ctypes.byref(desc))
        if rc != UMM_OK:
            raise RuntimeError(f"umm_free failed: rc={rc} ({self.errstr(rc)})")

    def fence(self) -> None:
        """落盘屏障：CPU 屏障 + SSD 数据面 msync(MS_SYNC)。"""
        self._lib.umm_fence()

    def invalidate(self, desc: ChunkDescriptor, offset: int, size: int) -> None:
        """丢弃对 chunk 区域的缓存页视图（共享盘读共享：对端落盘后本端刷盘）。"""
        if self._fn_invalidate is None:
            raise RuntimeError(
                "当前 libumm.so 未导出 umm_invalidate（需较新版本 UMM）"
            )
        rc = self._fn_invalidate(ctypes.byref(desc), offset, size)
        if rc != UMM_OK:
            raise RuntimeError(
                f"umm_invalidate failed: rc={rc} ({self.errstr(rc)})"
            )

    def read(self, desc: ChunkDescriptor, offset: int, size: int) -> bytes:
        buf = ctypes.create_string_buffer(size)
        rc = self._lib.umm_read(
            ctypes.byref(desc), offset, size, ctypes.cast(buf, ctypes.c_void_p)
        )
        if rc != UMM_OK:
            raise RuntimeError(
                f"umm_read failed: rc={rc} ({self.errstr(rc)})"
            )
        return buf.raw

    def write(self, desc: ChunkDescriptor, offset: int, data: bytes) -> None:
        if not data:
            return
        buf = ctypes.create_string_buffer(data)
        rc = self._lib.umm_write(
            ctypes.byref(desc), offset, len(data), ctypes.cast(buf, ctypes.c_void_p)
        )
        if rc != UMM_OK:
            raise RuntimeError(
                f"umm_write failed: rc={rc} ({self.errstr(rc)})"
            )

    def read_into(self, desc: ChunkDescriptor, offset: int, buf) -> int:
        """
        零拷贝读：直接读入调用方提供的可写 buffer（memoryview/bytearray/ctypes 数组）。
        返回读取字节数。buf 长度即读取长度。buffer 不可写或不是 C 连续的抛 ValueError。

        与 read() 的区别：read() 每次 create_string_buffer 分配临时内存并返回 bytes 拷贝；
        read_into() 通过 from_buffer 取得调用方 buffer 的原生指针，数据不经任何中间拷贝。
        """
        mv = memoryview(buf)
        if mv.nbytes == 0:
            return 0
        if mv.readonly:
            raise ValueError("read_into 需要可写 buffer（bytearray/memoryview/ctypes 数组）")
        try:
            # from_buffer 直接复用底层内存，不做数据拷贝；调用期间 c_buf 持有 buffer 引用
            c_buf = (ctypes.c_char * mv.nbytes).from_buffer(mv)
        except TypeError as e:
            raise ValueError(f"buffer 不支持零拷贝访问: {e}")
        rc = self._lib.umm_read(
            ctypes.byref(desc), offset, mv.nbytes, ctypes.cast(c_buf, ctypes.c_void_p)
        )
        if rc != UMM_OK:
            raise RuntimeError(
                f"umm_read failed: rc={rc} ({self.errstr(rc)})"
            )
        return mv.nbytes

    def write_from(self, desc: ChunkDescriptor, offset: int, buf) -> int:
        """
        零拷贝写：直接从调用方 buffer（memoryview/bytes/bytearray）写出，不做额外拷贝。
        返回写入字节数。

        可写 buffer 走 from_buffer 原生指针；bytes 等只读对象由 ctypes 直接引用其
        内部 buffer（调用期间 ctypes 保持引用，不复制数据）。其他只读 buffer 实在
        无法取指针时退化为一次 tobytes() 拷贝。
        """
        mv = memoryview(buf)
        if mv.nbytes == 0:
            return 0
        if mv.readonly:
            if isinstance(buf, bytes):
                data_ref = buf
            elif (
                isinstance(mv.obj, bytes)
                and mv.c_contiguous
                and mv.nbytes == len(mv.obj)
            ):
                # 包住 bytes 的完整 memoryview（偏移必为 0）：直接引用底层 bytes，零拷贝
                data_ref = mv.obj
            else:
                # 只读子视图等无法安全取指针的场景：兜底拷贝一次
                data_ref = mv.tobytes()
            # c_char_p 引用 bytes 内部 buffer，ctypes 在调用期间保持 data_ref 存活
            ptr = ctypes.cast(ctypes.c_char_p(data_ref), ctypes.c_void_p)
        else:
            try:
                c_buf = (ctypes.c_char * mv.nbytes).from_buffer(mv)
            except TypeError as e:
                raise ValueError(f"buffer 不支持零拷贝访问: {e}")
            ptr = ctypes.cast(c_buf, ctypes.c_void_p)
        rc = self._lib.umm_write(ctypes.byref(desc), offset, mv.nbytes, ptr)
        if rc != UMM_OK:
            raise RuntimeError(
                f"umm_write failed: rc={rc} ({self.errstr(rc)})"
            )
        return mv.nbytes

    def lookup_chunk(self, name: str) -> ChunkMetadata:
        meta = ChunkMetadata()
        rc = self._lib.umm_lookup_chunk(name.encode("utf-8"), ctypes.byref(meta))
        if rc != UMM_OK:
            raise RuntimeError(
                f"umm_lookup_chunk('{name}') failed: rc={rc} ({self.errstr(rc)})"
            )
        return meta

    def errstr(self, rc: int) -> str:
        s = self._lib.umm_error_string(rc)
        return s.decode("utf-8") if s else f"unknown({rc})"

    def tier_name(self, tier: int) -> str:
        s = self._lib.umm_tier_name(tier)
        return s.decode("utf-8") if s else f"tier_{tier}"
