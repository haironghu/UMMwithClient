"""
bmpclient/virtual_media_strategy.py — VirtualMedia 数据打散策略抽象与内建实现。

本模块定义数据打散策略的抽象基类 DataPlacementStrategy，以及若干内建策略：

- RoundRobinStrategy：顺序 round-robin 打散（默认）
- ConsistentHashStrategy：基于一致性哈希的打散，支持虚拟节点配置
- CustomStrategyPlugin：通过 importlib 加载用户自定义策略类

使用 create_strategy() 工厂函数根据策略名和配置构造策略实例。
"""

import hashlib
import importlib
import inspect
from abc import ABC, abstractmethod
from bisect import bisect_right
from typing import Dict, List, Type


class DataPlacementStrategy(ABC):
    """
    数据打散策略抽象基类。

    子类需要实现 locate(index) -> device_idx，用于根据全局条目索引
    决定该条目应写入哪个 SSD 设备。
    """

    def __init__(self, num_devices: int, config: dict):
        if num_devices <= 0:
            raise ValueError("num_devices must be positive")
        self._num_devices = num_devices
        self._config = config or {}

    @property
    def num_devices(self) -> int:
        """底层 SSD 设备数量。"""
        return self._num_devices

    @property
    def name(self) -> str:
        """策略名称，默认使用类名小写并去掉 Strategy 后缀。"""
        return self.__class__.__name__.lower().replace("strategy", "")

    @abstractmethod
    def locate(self, index: int) -> int:
        """
        根据条目索引返回目标设备索引。

        :param index: 全局条目索引（非负整数）
        :return: 目标 SSD 设备索引，0 <= device_idx < num_devices
        """
        raise NotImplementedError


class RoundRobinStrategy(DataPlacementStrategy):
    """
    顺序 round-robin 打散策略。

    第 index 条数据写入设备 index % num_devices。
    该策略与改造前 VirtualMedia 的默认行为完全一致。
    """

    def locate(self, index: int) -> int:
        if index < 0:
            raise ValueError("index must be non-negative")
        return index % self._num_devices


class ConsistentHashStrategy(DataPlacementStrategy):
    """
    一致性哈希打散策略。

    为每个设备创建若干虚拟节点（默认 150 个），构造有序哈希环。
    对条目索引计算哈希后，在环上顺时针找到最近的虚拟节点，
    返回该虚拟节点所属的设备索引。

    配置项：
    - virtual_nodes: 每个设备的虚拟节点数，默认 150
    - hash_seed: 哈希种子字符串前缀，默认 "vm_slot"
    """

    def __init__(self, num_devices: int, config: dict):
        super().__init__(num_devices, config)
        self._virtual_nodes = int(self._config.get("virtual_nodes", 150))
        if self._virtual_nodes <= 0:
            raise ValueError("virtual_nodes must be positive")
        self._hash_seed = str(self._config.get("hash_seed", "vm_slot"))
        self._ring: Dict[int, int] = {}
        self._keys: List[int] = []
        self._build_ring()

    def _build_ring(self) -> None:
        """构建一致性哈希环。"""
        for device_idx in range(self._num_devices):
            for vnode in range(self._virtual_nodes):
                key = self._hash(f"{self._hash_seed}:{device_idx}:{vnode}")
                self._ring[key] = device_idx
        self._keys = sorted(self._ring.keys())

    @staticmethod
    def _hash(value: str) -> int:
        """计算字符串的 32 位无符号哈希值。"""
        return int(hashlib.md5(value.encode("utf-8")).hexdigest(), 16) % (2 ** 32)

    def locate(self, index: int) -> int:
        if index < 0:
            raise ValueError("index must be non-negative")
        if not self._keys:
            raise RuntimeError("consistent hash ring is empty")

        h = self._hash(f"{self._hash_seed}:slot:{index}")
        pos = bisect_right(self._keys, h)
        if pos == len(self._keys):
            pos = 0
        key = self._keys[pos]
        return self._ring[key]


class CustomStrategyPlugin(DataPlacementStrategy):
    """
    通过 importlib 动态加载用户自定义策略类。

    配置项：
    - module: 自定义策略类所在的 Python 模块路径（如 my_package.my_strategy）
    - class: 自定义策略类名（如 MyStrategy）
    - options: 传递给自定义策略构造函数的额外配置字典（可选）
    """

    def __init__(self, num_devices: int, config: dict):
        super().__init__(num_devices, config)
        self._module_path = self._config.get("module")
        self._class_name = self._config.get("class")
        if not self._module_path or not self._class_name:
            raise ValueError(
                "custom strategy requires both 'module' and 'class' in config"
            )

        self._inner = self._load_strategy()

    def _load_strategy(self) -> DataPlacementStrategy:
        """动态加载用户策略类并实例化。"""
        try:
            module = importlib.import_module(self._module_path)
        except Exception as e:
            raise ImportError(
                f"failed to import custom strategy module {self._module_path}: {e}"
            ) from e

        strategy_cls = getattr(module, self._class_name, None)
        if strategy_cls is None:
            raise ImportError(
                f"custom strategy class {self._class_name} not found in module {self._module_path}"
            )

        if not inspect.isclass(strategy_cls) or not issubclass(
            strategy_cls, DataPlacementStrategy
        ):
            raise TypeError(
                f"custom strategy class {self._class_name} must be a subclass of DataPlacementStrategy"
            )

        options = self._config.get("options", {})
        return strategy_cls(self._num_devices, options)

    @property
    def name(self) -> str:
        """返回内部策略的名称。"""
        return f"custom:{self._inner.name}"

    def locate(self, index: int) -> int:
        return self._inner.locate(index)


# 内建策略注册表
_STRATEGY_REGISTRY: Dict[str, Type[DataPlacementStrategy]] = {
    "round_robin": RoundRobinStrategy,
    "consistent_hash": ConsistentHashStrategy,
    "custom": CustomStrategyPlugin,
}


def register_strategy(name: str, strategy_cls: Type[DataPlacementStrategy]) -> None:
    """
    注册自定义策略类到工厂。

    :param name: 策略名
    :param strategy_cls: DataPlacementStrategy 子类
    """
    if not inspect.isclass(strategy_cls) or not issubclass(
        strategy_cls, DataPlacementStrategy
    ):
        raise TypeError("strategy_cls must be a subclass of DataPlacementStrategy")
    _STRATEGY_REGISTRY[name] = strategy_cls


def list_strategies() -> List[str]:
    """返回所有已注册的策略名列表。"""
    return list(_STRATEGY_REGISTRY.keys())


def create_strategy(
    name: str, num_devices: int, config: dict = None
) -> DataPlacementStrategy:
    """
    根据策略名构造策略实例。

    :param name: 策略名，如 "round_robin"、"consistent_hash"、"custom"
    :param num_devices: SSD 设备数量
    :param config: 策略配置字典
    :return: DataPlacementStrategy 实例
    :raises ValueError: 策略名未知
    """
    config = config or {}
    if name not in _STRATEGY_REGISTRY:
        raise ValueError(
            f"unknown strategy '{name}', available: {list_strategies()}"
        )
    return _STRATEGY_REGISTRY[name](num_devices, config)
