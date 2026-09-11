# -*- coding: utf-8 -*-
"""
bmpclient/virtual_media.py — VirtualMedia：稀疏 KV cache 专用介质层。

合并重构后（docs/07），VirtualMedia 只服务稀疏注意力 KV cache 卸载/加载
场景，职责三件套：

- 盘感知：umm_get_topology 发现在线 SSD、alloc_on_device 分配每盘 extent；
- 打散策略：可插拔 PlacementStrategy（默认 position_hash），
  key -> device_idx；稀疏 KV 场景 key = (layer_id, token_idx)；
- 数据路径：同盘单元聚合成段（super page）缓冲，写满以一次段大小连续
  IO 下盘；读侧 IN_BUF 透明（缓冲 memcpy）+ 偏移相邻合并 + 跨盘并行。

寻址一律 (device, offset)，与 UMM 数据面同构；不再有旧版"全局自增索引"
的 save/read API（已删除，见 docs/07 §6）。

设计文档：bmpclient/docs/06_稀疏注意力KV卸载数据排布设计.md、
         bmpclient/docs/07_VirtualMedia合并设计_稀疏KV专用介质.md。
"""

import threading
from typing import Any, Dict, List, Optional, Tuple, Union

from bmpclient._vm_segment import DeviceSegmentManager, SegmentFullError
from bmpclient.concurrent_io import ConcurrentIOEngine, IOAddress, IORequest
from bmpclient.umm_client import ChunkDescriptor, UMMLib, UMM_TIER_SSD
from bmpclient.virtual_media_config import (
    get_strategy_name,
    get_strategy_options,
    load_config,
)
from bmpclient.virtual_media_strategy import PlacementStrategy, create_strategy

__all__ = ["VirtualMedia", "SegmentFullError"]


class VirtualMedia:
    """
    稀疏 KV cache 专用介质：盘感知 + 打散策略 + 段聚合写路径。

    :param lib: 已初始化的 UMMLib 实例（或 FakeUMMLib）
    :param unit_size: 排布单元定长（4KB 对齐）
    :param capacity_per_device: 每盘 extent 字节数（必须是本盘段大小整数倍）
    :param strategy: 打散策略。None 读配置文件；str 为策略名；
        PlacementStrategy 实例直接使用
    :param sp_bytes: 段（super page）全局默认大小；None 读配置
    :param sp_bytes_per_device: 按盘覆盖段大小 {device_idx: bytes}
    :param num_workers: read_batch 兜底读的跨盘并行线程数
    :param config_path: 配置文件路径（None 用默认 virtual_media.json）
    """

    def __init__(
        self,
        lib: UMMLib,
        unit_size: int,
        capacity_per_device: Union[int, Dict[int, int]],
        strategy: Union[str, PlacementStrategy, None] = None,
        sp_bytes: Optional[int] = None,
        sp_bytes_per_device: Optional[Dict[int, int]] = None,
        num_workers: int = 4,
        config_path: Optional[str] = None,
        read_merge: bool = True,
        max_read_bytes: Optional[int] = None,
    ):
        if max_read_bytes is not None and (unit_size <= 0 or max_read_bytes < unit_size or
                                           max_read_bytes % unit_size):
            raise ValueError("max_read_bytes 必须是 unit_size 的正整数倍")
        self._read_merge = read_merge
        self._max_read_bytes = max_read_bytes
        cfg = load_config(config_path)
        self._lib = lib
        self._unit_size = unit_size
        self._cfg = cfg

        devices = self._list_ssd_devices()
        if not devices:
            raise RuntimeError("无在线 SSD 设备")
        self._devices = devices

        default_sp = sp_bytes if sp_bytes is not None else int(cfg.get("super_page_bytes"))
        sp_override = sp_bytes_per_device if sp_bytes_per_device is not None else {
            int(k): int(v)
            for k, v in cfg.get("super_page_bytes_per_device", {}).items()
        }
        self._mgrs: List[DeviceSegmentManager] = []
        self._strategy = self._resolve_strategy(strategy, cfg, len(devices))
        try:
            for d in devices:
                capacity = capacity_per_device[d] if isinstance(capacity_per_device, dict) else capacity_per_device
                desc = lib.alloc_on_device(capacity, UMM_TIER_SSD, d)
                try:
                    mgr = DeviceSegmentManager(lib, desc, d, sp_override.get(d, default_sp), unit_size)
                except BaseException:
                    lib.free(desc)
                    raise
                self._mgrs.append(mgr)
        except BaseException:
            for mgr in self._mgrs:
                lib.free(mgr.desc)
            raise
        self._engine = ConcurrentIOEngine(lib, num_workers=num_workers)
        self._write_lock = threading.Lock()   # write 单写者串行化
        self._closed = False

    # ------------------------------------------------------------------
    # 初始化辅助
    # ------------------------------------------------------------------

    def _list_ssd_devices(self) -> List[int]:
        """查询当前在线 SSD 设备索引列表（从 0 开始编号）。"""
        topo = self._lib.get_topology()
        count = 0
        for i in range(topo.num_resources):
            res = topo.resources[i]
            if res.tier == UMM_TIER_SSD and res.online:
                count += 1
        return list(range(count))

    def _resolve_strategy(self, strategy, cfg, num_devices) -> PlacementStrategy:
        if strategy is None:
            return create_strategy(
                get_strategy_name(cfg), num_devices, get_strategy_options(cfg)
            )
        if isinstance(strategy, str):
            return create_strategy(strategy, num_devices, {})
        if isinstance(strategy, PlacementStrategy):
            if strategy.num_devices != num_devices:
                raise ValueError(
                    f"strategy num_devices ({strategy.num_devices}) != "
                    f"在线 SSD 数 ({num_devices})"
                )
            return strategy
        raise TypeError("strategy 必须是 None / 策略名 / PlacementStrategy 实例")

    # ------------------------------------------------------------------
    # 打散策略（key -> device）
    # ------------------------------------------------------------------

    @property
    def strategy(self) -> PlacementStrategy:
        return self._strategy

    @property
    def device_count(self) -> int:
        return len(self._devices)

    @property
    def unit_size(self) -> int:
        return self._unit_size

    def locate(self, key: Any) -> int:
        """由策略计算 key 的目标盘号。稀疏 KV 场景 key = (layer_id, token_idx)。"""
        return self._strategy.locate(key)

    def locate_batch(self, keys: List[Any]) -> List[int]:
        """批量 locate（地址规划流水线第①步）。"""
        return self._strategy.locate_batch(keys)

    # ------------------------------------------------------------------
    # 写路径（CPU 提交，不入图）
    # ------------------------------------------------------------------

    def _ensure_open(self) -> None:
        if self._closed:
            raise RuntimeError("VirtualMedia 已关闭")

    def write(self, key: Any, data: bytes) -> int:
        """
        写一个单元：locate(key) 分盘 → 追加该盘段缓冲（满则一次段大小
        连续 IO 下盘）。返回 extent 内字节偏移（调用方入元数据）。
        段耗尽抛 SegmentFullError。
        """
        self._ensure_open()
        d = self._strategy.locate(key)
        with self._write_lock:
            return self._mgrs[d].append(data)

    def flush(self) -> None:
        """强制刷所有盘的半满段缓冲。"""
        self._ensure_open()
        with self._write_lock:
            for mgr in self._mgrs:
                mgr.flush()

    # ------------------------------------------------------------------
    # 读路径支撑（生产读由 GPU 直通算子执行；这里供地址规划与兜底）
    # ------------------------------------------------------------------

    def extent_base(self, device: int) -> int:
        """该盘 extent 在存储侧的基址（盘侧地址 = extent_base + offset）。

        若适配层提供 resolve_ssd_extent_base，则使用其设备地址解析器。
        原生 SSD pool 回放适配层返回窗口起点加 extent 的设备内偏移。
        未提供解析器的旧 UMMLib 路径仍返回 0 占位，不能据此用于直通；
        CPU read_batch 使用 descriptor 读写，不依赖此函数。
        """
        self._check_device(device)
        resolver = getattr(self._lib, 'resolve_ssd_extent_base', None)
        if resolver is not None:
            return resolver(self._mgrs[device].desc, self._devices[device])
        return 0

    def is_buffered(self, device: int, offset: int) -> bool:
        """该偏移是否仍在主机段聚合缓冲（未下盘）。plan 兜底判定用。"""
        self._ensure_open()
        self._check_device(device)
        return self._mgrs[device].is_buffered(offset)

    def read_buffered(self, device: int, offset: int) -> Optional[bytes]:
        """读未下盘单元（在缓冲返回数据，否则 None——调用方回退盘读）。"""
        self._check_device(device)
        return self._mgrs[device].read_buffer(offset, self._unit_size)

    def read_batch(
        self, items: List[Tuple[int, int]], outs: List[memoryview]
    ) -> None:
        """
        CPU 兜底批量读（联调/无直通硬件环境用；生产读路径不走这里）。
        items = [(device, offset)]，outs[i] 对应 items[i]，长度 >= unit_size。
        内部：IN_BUF 从缓冲 memcpy；盘读按盘分桶、offset 排序、相邻合并、
        ConcurrentIOEngine 跨盘并行。
        """
        self._ensure_open()
        if len(items) != len(outs):
            raise ValueError("items 与 outs 长度不一致")
        buckets: Dict[int, List[Tuple[int, int]]] = {}
        for pos, (d, off) in enumerate(items):
            self._check_device(d)
            if outs[pos].nbytes < self._unit_size:
                raise ValueError("out buffer 不足 unit_size")
            data = self._mgrs[d].read_buffer(off, self._unit_size)
            if data is not None:
                outs[pos][: self._unit_size] = data
            else:
                buckets.setdefault(d, []).append((pos, off))

        requests: List[IORequest] = []
        staged: List[Tuple[bytearray, List[Tuple[int, int]]]] = []
        for d, group in buckets.items():
            group.sort(key=lambda x: x[1])
            desc = self._mgrs[d].desc
            for run in self._merge_runs(group):
                start_off = run[0][1]
                staging = bytearray(run[-1][1] + self._unit_size - start_off)
                requests.append(IORequest(
                    IOAddress.from_descriptor(desc, start_off, len(staging)),
                    memoryview(staging),
                ))
                staged.append((staging, [(p, o - start_off) for p, o in run]))
        if requests:
            self._engine.read_batch(requests)
            for staging, pieces in staged:
                for pos, local in pieces:
                    outs[pos][: self._unit_size] = staging[
                        local:local + self._unit_size
                    ]

    def _merge_runs(
        self, items: List[Tuple[int, int]]
    ) -> List[List[Tuple[int, int]]]:
        """把按 offset 排序的 (pos, offset) 列表中相邻单元合并为连续读段。"""
        runs: List[List[Tuple[int, int]]] = []
        for pos, off in items:
            if (self._read_merge and runs and
                off == runs[-1][-1][1] + self._unit_size and
                (self._max_read_bytes is None or
                 off + self._unit_size - runs[-1][0][1] <= self._max_read_bytes)):
                runs[-1].append((pos, off))
            else:
                runs.append([(pos, off)])
        return runs

    def _check_device(self, device: int) -> None:
        if not 0 <= device < len(self._mgrs):
            raise ValueError(f"device 越界: {device}")

    # ------------------------------------------------------------------
    # 回收与生命周期
    # ------------------------------------------------------------------

    def release(self, device: int, offset: int) -> None:
        """释放一个单元：递减所属段引用计数，归零段回空闲池。"""
        self._ensure_open()
        self._check_device(device)
        self._mgrs[device].release(offset)

    def stats(self) -> dict:
        """各盘空闲段数与缓冲单元数（观测用）。"""
        return {
            "devices": len(self._mgrs),
            "free_segments": [m.num_free_segments for m in self._mgrs],
            "buffered_units": [m.buffered_units for m in self._mgrs],
        }

    def close(self) -> None:
        """释放各盘 extent 与并发引擎（未 flush 的半满段数据随之丢失）。"""
        if self._closed:
            return
        self._closed = True
        self._engine.close()
        for mgr in self._mgrs:
            self._lib.free(mgr.desc)

    def __enter__(self) -> "VirtualMedia":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> bool:
        self.close()
        return False
