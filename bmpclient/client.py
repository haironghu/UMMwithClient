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
                 tier_aware: bool = False,
                 peer_nodes: str = "",
                 rpc_token: str = "",
                 ssd_owner_node: int = 0xFF,
                 data_max_io: int = 0,
                 memory_size: int = 0,
                 local_mem_as_dram: int = 0,
                 mem_device: str = ""):
        """
        :param ssd_device:  兼容旧版：单个 SSD 设备路径（如 "/tmp/ssd.raw"）
        :param ssd_devices: 新版多设备列表：[(path, size_bytes), ...]
        :param tier_aware:  True 时启用 tier 路由模式（transport 置空），
                            SSD tier 数据面走 tier_router → transport_ssd；
                            False 保持 legacy mock transport（仅 dram/CXL）
        :param peer_nodes:  Phase 1 集群数据面对等表 "node:host:port,..."
                            （跨节点部署必填，如 "0:10.0.0.11:20002"）
        :param rpc_token:   共享密钥，须与服务端 umms 配置一致（若启用）
        :param ssd_owner_node: 旧服务端（alloc 响应无属主）时的属主回退；
                            0xFF = 用本节点 id（单节点旧行为）
        :param data_max_io: 单数据面 RPC payload 上限（0 = 默认 1MB）
        :param memory_size: 客户端本地内存数据面（malloc 后备）容量，
                            须 >= 本进程内存层 chunk 总量（混合池实验用）；
                            0 = 默认 64MB
        :param local_mem_as_dram: 1 = 本地内存数据面注册 DRAM tier(tier=0)
                            （Phase 2 混合池路线B，无 CXL 硬件）；
                            0 = 默认注册 CXL tier（mock/malloc 后备，旧行为）
        :param mem_device: 内存层后备设备（Phase 2.5 共享内存窗口，如
                            virtio-pmem 的 /dev/pmem0）；空 = malloc/mock
                            私有后备。写入 UMMConfig.cxl_device 字段（两个
                            tier 分支都读它；复用是为避免 ABI 变更）。
                            注意：容量须 >= memory_size，且与 umms 的
                            memory_device 指向同一共享窗口。
        """
        self.lib = UMMLib()
        cfg = UMMConfig()
        cfg.transport = b"" if tier_aware else b"mock"
        cfg.consistency_model = b"hardware"
        cfg.memory_size = memory_size if memory_size > 0 else 64 * 1024 * 1024
        cfg.meta_server_addr = meta_addr.encode("utf-8")
        cfg.mem_server_addr = mem_addr.encode("utf-8")
        cfg.ssd_device = ssd_device.encode("utf-8") if ssd_device else b""
        cfg.my_node_id = node_id
        cfg.peer_nodes = peer_nodes.encode("utf-8") if peer_nodes else b""
        cfg.rpc_token = rpc_token.encode("utf-8") if rpc_token else b""
        cfg.ssd_owner_node = ssd_owner_node
        cfg.data_max_io = data_max_io
        cfg.local_mem_as_dram = 1 if local_mem_as_dram else 0
        if mem_device:
            cfg.cxl_device = mem_device.encode("utf-8")

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

    def create_chunk_on_tier(self, size: int, tier: int) -> ChunkDescriptor:
        """按 tier 编号分配（0=DRAM 1=CXL 2=SSD）——混合池实验用。

        RPC 模式下分配权威是 umms（全局 offset）；数据面按 GPA tier 位
        路由：tier=1/0 落客户端本地 malloc 后备，tier=2 落本机 SSD 设备。
        """
        if not 0 <= tier <= 2:
            raise ValueError(f"非法 tier: {tier}")
        return self.lib.alloc_tiered(size, tier)

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

    def flush(self) -> None:
        """落盘屏障：确保此前所有 SSD 写入到达后备存储（umm_fence）。

        共享盘读共享场景：写完调用本方法后，另一节点 invalidate 再读
        即可看到数据。"""
        self.lib.fence()

    def invalidate_chunk(self, desc: ChunkDescriptor, offset: int,
                         size: int) -> None:
        """丢弃对 chunk 区域的缓存页视图（对端落盘后的"刷盘"动作）。"""
        self.lib.invalidate(desc, offset, size)

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
