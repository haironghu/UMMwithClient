"""
bmpclient/client.py — UMM C API 客户端封装。

替代原来的 MemoryServiceClient（HTTP 调用），改为通过 ctypes 调用 libumm.so。
"""

from typing import Optional, List, Tuple

from bmpclient.umm_client import (
    UMMLib,
    UMMConfig,
    SsdDeviceConfig,
    ChunkDescriptor,
    StorageTopology,
    UMM_TIER_SSD,
)


class UMMServiceClient:
    """封装对 UMM 的 C API 调用，提供 chunk 生命周期管理。"""

    def __init__(self, meta_addr: str, mem_addr: str, node_id: int = 0,
                 ssd_device: str = "",
                 ssd_devices: List[Tuple[str, int]] = None,
                 tier_aware: bool = False):
        """
        :param ssd_device:  兼容旧版：单个 SSD 设备路径（如 "/tmp/ssd.raw"）
        :param ssd_devices: 新版多设备列表：[(path, size_bytes), ...]
        :param tier_aware:  True 时启用 tier 路由模式（transport 置空），
                            SSD tier 数据面走 tier_router → transport_ssd；
                            False 保持 legacy mock transport（仅 dram/CXL）
        """
        self.lib = UMMLib()
        cfg = UMMConfig()
        cfg.transport = b"" if tier_aware else b"mock"
        cfg.consistency_model = b"hardware"
        cfg.memory_size = 64 * 1024 * 1024
        cfg.meta_server_addr = meta_addr.encode("utf-8")
        cfg.mem_server_addr = mem_addr.encode("utf-8")
        cfg.ssd_device = ssd_device.encode("utf-8") if ssd_device else b""
        cfg.my_node_id = node_id

        # 多 SSD 设备配置
        if ssd_devices:
            num = min(len(ssd_devices), 16)
            cfg.num_ssd_devices = num
            for i in range(num):
                path, size = ssd_devices[i]
                cfg.ssd_devices[i].path = path.encode("utf-8")
                cfg.ssd_devices[i].size = size
        else:
            cfg.num_ssd_devices = 0

        rc = self.lib.init(cfg)
        if rc != 0:
            raise RuntimeError(
                f"umm_init failed: rc={rc} ({self.lib.errstr(rc)})"
            )

    def create_chunk(
        self,
        size: int,
        media_type: str = "dram",
        device_idx: Optional[int] = None,
    ) -> ChunkDescriptor:
        """
        分配一个 UMM Chunk。
        dram → umm_alloc()（默认 CXL tier）
        ssd  → umm_alloc_tiered(UMM_TIER_SSD)  或
                umm_alloc_on_device(UMM_TIER_SSD, device_idx) 当 device_idx 指定时
        """
        if media_type == "ssd":
            if device_idx is not None:
                return self.lib.alloc_on_device(size, UMM_TIER_SSD, device_idx)
            return self.lib.alloc_tiered(size, UMM_TIER_SSD)
        else:
            if device_idx is not None:
                raise ValueError("device_idx 仅在 media_type='ssd' 时支持")
            return self.lib.alloc(size)

    def enable_ssd(self, device_path: str, capacity: int) -> None:
        """注册 SSD tier 存储设备（需 tier_aware=True 构造）。

        RPC 模式下：上报 ummD 全局拓扑 + 在 client 进程本地建立
        SSD 数据面（tier_router → transport_ssd → 设备）。
        必须在 init 之后、任何数据 I/O 之前调用。
        文件设备例 "/tmp/ssd.raw"；libnvm 设备例
        "libnvm:/dev/libnvm_helper0@1+0x40000000"（带窗口基址）。
        """
        self.lib.register_storage_tier(UMM_TIER_SSD, device_path, capacity)

    def get_topology(self) -> StorageTopology:
        """查询当前管理的存储拓扑（CXL + 每个 SSD 设备）。"""
        return self.lib.get_topology()

    def delete_chunk(self, desc: ChunkDescriptor) -> None:
        """释放一个 UMM Chunk。"""
        self.lib.free(desc)

    def read_chunk(self, desc: ChunkDescriptor, offset: int, size: int) -> bytes:
        """从 Chunk 的指定偏移读取数据。"""
        return self.lib.read(desc, offset, size)

    def write_chunk(self, desc: ChunkDescriptor, offset: int, data: bytes) -> None:
        """向 Chunk 的指定偏移写入数据。"""
        self.lib.write(desc, offset, data)

    def lookup_chunk(self, name: str):
        """按名称查找 Chunk 元数据。"""
        return self.lib.lookup_chunk(name)

    def get_device_list(self) -> List[dict]:
        """
        返回当前管理的设备列表，每条记录包含：
        tier, device_path, capacity, base_offset, online

        布局兼容：ummD 按 tier 稀疏存放、mem_service 返回紧凑数组，
        因此遍历全部槽位并过滤 online，而非仅取前 num_resources 项。
        """
        topo = self.get_topology()
        devices = []
        for res in topo.resources:
            if not res.online:
                continue
            devices.append(
                {
                    "tier": res.tier,
                    "device_path": res.device_path.decode("utf-8").rstrip("\x00"),
                    "capacity": res.capacity,
                    "base_offset": res.base_offset,
                    "online": bool(res.online),
                }
            )
        return devices

    def close(self) -> None:
        """关闭 UMM 会话。"""
        self.lib.deinit()
