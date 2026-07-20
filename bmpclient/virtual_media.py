"""
bmpclient/virtual_media.py — 跨多 SSD 设备的定长数据存储抽象。

VirtualMedia 将总容量 size 均匀分布在所有在线 SSD 设备上，
以固定粒度 granularity 保存数据，并通过可插拔的数据打散策略
决定每条数据落到的具体 SSD 设备。

默认策略为 round-robin，与改造前行为完全一致。其他可选策略包括：
- consistent_hash：一致性哈希
- custom：通过配置文件加载用户自定义策略
"""

import threading
from dataclasses import dataclass
from typing import List, Optional, Tuple, Union

from bmpclient.umm_client import UMMLib, ChunkDescriptor, UMM_TIER_SSD
from bmpclient.virtual_media_config import get_strategy_name, get_strategy_options, load_config
from bmpclient.virtual_media_strategy import DataPlacementStrategy, create_strategy


class VirtualMediaFullError(Exception):
    """VirtualMedia 的所有条目槽位已写满。"""
    pass


@dataclass
class DeviceExtent:
    """VirtualMedia 在单个 SSD 设备上占用的一段空间。"""
    device_idx: int           # SSD 设备索引
    desc: ChunkDescriptor     # UMM 分配返回的 ChunkDescriptor（含 GPA、size）


class VirtualMedia:
    """
    抽象底层多段 SSD 设备提供的定长数据存储空间。

    创建时按 size 在所有在线 SSD 设备间均匀分配；
    写入时通过配置的数据打散策略将每条数据映射到不同 SSD 设备；
    读取时按保存时返回的索引定位数据。

    数据打散策略可通过以下三种方式指定（优先级从高到低）：
    1. VirtualMedia 构造函数传入 strategy 参数（策略实例或策略名）
    2. 读取 bmpclient/config/virtual_media.json 配置文件
    3. 默认使用 round-robin 策略
    """

    def __init__(
        self,
        lib: UMMLib,
        size: int,
        granularity: int,
        strategy: Union[str, DataPlacementStrategy, None] = None,
    ):
        """
        :param lib: 已初始化的 UMMLib 实例
        :param size: VirtualMedia 总容量（字节），必须能被在线 SSD 设备数整除
        :param granularity: 单条数据固定长度（字节），必须能整除每设备分配大小
        :param strategy: 数据打散策略。可选：
            - None：读取配置文件决定策略
            - str：策略名，如 "round_robin"、"consistent_hash"、"custom"
            - DataPlacementStrategy 实例：用户自定义策略对象
        """
        if size <= 0:
            raise ValueError("size must be positive")
        if granularity <= 0:
            raise ValueError("granularity must be positive")
        if size % granularity != 0:
            raise ValueError(
                f"size ({size}) must be divisible by granularity ({granularity})"
            )

        self._lib = lib
        self._size = size
        self._granularity = granularity
        self._lock = threading.Lock()
        self._next_slot = 0
        self._extents: List[DeviceExtent] = []
        self._index_map: List[Tuple[int, int]] = []
        self._device_write_counts: List[int] = []

        # 查询在线 SSD 设备
        ssd_devices = self._list_ssd_devices()
        num_devices = len(ssd_devices)
        if num_devices == 0:
            raise RuntimeError("No online SSD devices available")

        if size % num_devices != 0:
            raise ValueError(
                f"size ({size}) must be divisible by SSD device count ({num_devices})"
            )

        device_size = size // num_devices
        if device_size % granularity != 0:
            raise ValueError(
                f"per-device size ({device_size}) must be divisible by granularity ({granularity})"
            )

        # 在每个 SSD 设备上分配 device_size 空间
        for device_idx in sorted(ssd_devices):
            desc = lib.alloc_on_device(device_size, UMM_TIER_SSD, device_idx)
            self._extents.append(DeviceExtent(device_idx=device_idx, desc=desc))

        # 按 device_idx 排序，确保设备顺序稳定
        self._extents.sort(key=lambda e: e.device_idx)
        self._device_write_counts = [0] * num_devices

        # 初始化数据打散策略
        self._strategy = self._resolve_strategy(strategy, num_devices)

    def _list_ssd_devices(self) -> List[int]:
        """查询当前在线的 SSD 设备索引列表（从 0 开始编号）。"""
        topo = self._lib.get_topology()
        ssd_count = 0
        for i in range(topo.num_resources):
            res = topo.resources[i]
            if res.tier == UMM_TIER_SSD and res.online:
                ssd_count += 1
        return list(range(ssd_count))

    def _resolve_strategy(
        self,
        strategy: Union[str, DataPlacementStrategy, None],
        num_devices: int,
    ) -> DataPlacementStrategy:
        """根据用户输入或配置文件解析最终使用的数据打散策略。"""
        if strategy is None:
            cfg = load_config()
            name = get_strategy_name(cfg)
            options = get_strategy_options(cfg)
            return create_strategy(name, num_devices, options)

        if isinstance(strategy, str):
            return create_strategy(strategy, num_devices, {})

        if isinstance(strategy, DataPlacementStrategy):
            if strategy.num_devices != num_devices:
                raise ValueError(
                    f"strategy num_devices ({strategy.num_devices}) does not match "
                    f"online SSD device count ({num_devices})"
                )
            return strategy

        raise TypeError(
            "strategy must be None, a strategy name string, or a DataPlacementStrategy instance"
        )

    @property
    def capacity(self) -> int:
        """VirtualMedia 总容量（字节）。"""
        return self._size

    @property
    def granularity(self) -> int:
        """单条数据固定长度（字节）。"""
        return self._granularity

    @property
    def slot_count(self) -> int:
        """可存储的条目总数。"""
        return self._size // self._granularity

    @property
    def written_count(self) -> int:
        """已写入条目数。"""
        with self._lock:
            return self._next_slot

    @property
    def device_count(self) -> int:
        """底层 SSD 设备数量。"""
        return len(self._extents)

    @property
    def strategy(self) -> DataPlacementStrategy:
        """当前使用的数据打散策略。"""
        return self._strategy

    def save(self, data: bytes) -> int:
        """
        保存一条定长数据。

        :param data: 待保存数据，长度必须等于 granularity
        :return: 本次写入的条目索引
        :raises ValueError: 数据长度不等于 granularity
        :raises VirtualMediaFullError: 所有槽位已满
        """
        if len(data) != self._granularity:
            raise ValueError(
                f"data length ({len(data)}) must equal granularity ({self._granularity})"
            )

        with self._lock:
            if self._next_slot >= self.slot_count:
                raise VirtualMediaFullError(
                    f"VirtualMedia is full: {self._next_slot}/{self.slot_count} slots used"
                )

            index = self._next_slot
            num_devices = len(self._extents)
            device_idx = self._strategy.locate(index)
            slot_within_device = self._device_write_counts[device_idx]
            slots_per_device = self.slot_count // num_devices

            if slot_within_device >= slots_per_device:
                raise VirtualMediaFullError(
                    f"device {device_idx} is full: "
                    f"{slot_within_device}/{slots_per_device} slots used"
                )

            offset_within_device = slot_within_device * self._granularity

            extent = self._extents[device_idx]
            self._lib.write(extent.desc, offset_within_device, data)

            self._device_write_counts[device_idx] += 1
            self._index_map.append((device_idx, slot_within_device))
            self._next_slot += 1
            return index

    def read(self, index: int) -> bytes:
        """
        按条目索引读取数据。

        :param index: 条目索引，必须 0 <= index < written_count
        :return: 读取到的数据（长度为 granularity）
        :raises ValueError: 索引越界
        """
        if index < 0:
            raise ValueError("index must be non-negative")

        with self._lock:
            if index >= self._next_slot:
                raise ValueError(
                    f"index {index} not written yet (written_count={self._next_slot})"
                )

            device_idx, slot_within_device = self._index_map[index]
            offset_within_device = slot_within_device * self._granularity

            extent = self._extents[device_idx]
            return self._lib.read(extent.desc, offset_within_device, self._granularity)

    def close(self) -> None:
        """释放 VirtualMedia 占用的所有底层 SSD Chunk。"""
        with self._lock:
            for extent in self._extents:
                self._lib.free(extent.desc)
            self._extents.clear()
            self._index_map.clear()
            self._device_write_counts.clear()
            self._next_slot = 0

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False
