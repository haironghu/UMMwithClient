# -*- coding: utf-8 -*-
"""
bmpclient/virtual_media_strategy.py — VirtualMedia 数据打散策略框架。

合并重构后（docs/07），策略接口从"写入序号 -> 盘号"升级为
**语义键 -> 盘号**：`locate(key)`，key 由各场景定义。本介质只服务稀疏
KV cache 场景，key = (layer_id, token_idx)，内建策略为确定性位置哈希：

    device(t, l) = ((t * STEP_IDX + l * STEP_LAYER) % PRIME) % N_SSD

性质与参数约束见 docs/06_稀疏注意力KV卸载数据排布设计.md §3：
- 性质一：连续 N_SSD 个 token 恰好覆盖全部盘（适配 topk 局部性）；
- 性质二：同一 token 连续 N_SSD 层恰好覆盖全部盘（适配层间相似性）。
"""

import inspect
from abc import ABC, abstractmethod
from typing import Any, Dict, List, Type

DEFAULT_STEP_IDX = 17
DEFAULT_STEP_LAYER = 23
DEFAULT_PRIME = 2147483647  # 2^31 - 1，梅森质数


def is_prime(n: int) -> bool:
    """试除法判质数（参数校验用，非热路径）。"""
    if n < 2:
        return False
    if n % 2 == 0:
        return n == 2
    i = 3
    while i * i <= n:
        if n % i == 0:
            return False
        i += 2
    return True


class PlacementStrategy(ABC):
    """
    打散策略抽象基类：locate(key) -> device_idx。

    key 由场景定义；稀疏 KV 场景为 (layer_id, token_idx)。
    """

    def __init__(self, num_devices: int, config: dict):
        if num_devices <= 0:
            raise ValueError("num_devices must be positive")
        self._num_devices = num_devices
        self._config = config or {}

    @property
    def num_devices(self) -> int:
        return self._num_devices

    @property
    def name(self) -> str:
        return self.__class__.__name__.lower().replace("strategy", "")

    @abstractmethod
    def locate(self, key: Any) -> int:
        """计算 key 的目标盘号，0 <= device_idx < num_devices。"""
        raise NotImplementedError

    def locate_batch(self, keys: List[Any]) -> List[int]:
        """批量 locate；子类可覆写做向量化优化。"""
        return [self.locate(k) for k in keys]


class PositionHashStrategy(PlacementStrategy):
    """
    确定性位置哈希打散（稀疏 KV 场景默认策略）。

    key = (layer_id, token_idx)。凭 key 无状态计算盘号，读路径
    "数据在哪块盘"零元数据。

    配置项：
    - step_idx / step_layer：大于 num_devices 的互异质数
    - prime：更大的质数；若同时给出 max_token_idx / max_layer_id，
      要求 prime > max_token_idx*step_idx + max_layer_id*step_layer
      （定义域内无取模回绕）
    """

    def __init__(self, num_devices: int, config: dict):
        super().__init__(num_devices, config)
        self._step_idx = int(self._config.get("step_idx", DEFAULT_STEP_IDX))
        self._step_layer = int(self._config.get("step_layer", DEFAULT_STEP_LAYER))
        self._prime = int(self._config.get("prime", DEFAULT_PRIME))

        n = num_devices
        for name, step in (("step_idx", self._step_idx),
                           ("step_layer", self._step_layer)):
            if not is_prime(step):
                raise ValueError(f"{name} 必须是质数: {step}")
            if step <= n:
                raise ValueError(f"{name} ({step}) 必须大于盘数 ({n})")
        if self._step_idx == self._step_layer:
            raise ValueError("step_idx 与 step_layer 必须互异")
        if not is_prime(self._prime):
            raise ValueError(f"prime 必须是质数: {self._prime}")
        if self._prime <= max(self._step_idx, self._step_layer):
            raise ValueError(f"prime ({self._prime}) 必须大于两个步进参数")

        max_t = self._config.get("max_token_idx")
        max_l = self._config.get("max_layer_id")
        if max_t is not None and max_l is not None:
            domain_max = int(max_t) * self._step_idx + int(max_l) * self._step_layer
            if self._prime <= domain_max:
                raise ValueError(
                    f"prime ({self._prime}) 不大于定义域上界 {domain_max}，"
                    "定义域内会发生取模回绕，性质一/二不再严格成立"
                )

    def locate(self, key) -> int:
        layer_id, token_idx = key
        if token_idx < 0 or layer_id < 0:
            raise ValueError("token_idx / layer_id 必须非负")
        return ((token_idx * self._step_idx + layer_id * self._step_layer)
                % self._prime) % self._num_devices

    def locate_batch(self, keys) -> List[int]:
        """同一层的批量 key 可复用 layer 项（decode 定位第①步）。"""
        step_t, step_l, prime, n = (
            self._step_idx, self._step_layer, self._prime, self._num_devices)
        out = []
        for layer_id, token_idx in keys:
            out.append(((token_idx * step_t + layer_id * step_l) % prime) % n)
        return out


# 内建策略注册表
_STRATEGY_REGISTRY: Dict[str, Type[PlacementStrategy]] = {
    "position_hash": PositionHashStrategy,
}


def register_strategy(name: str, strategy_cls: Type[PlacementStrategy]) -> None:
    """注册自定义策略类到工厂。"""
    if not inspect.isclass(strategy_cls) or not issubclass(
        strategy_cls, PlacementStrategy
    ):
        raise TypeError("strategy_cls must be a subclass of PlacementStrategy")
    _STRATEGY_REGISTRY[name] = strategy_cls


def list_strategies() -> List[str]:
    """返回所有已注册的策略名列表。"""
    return list(_STRATEGY_REGISTRY.keys())


def create_strategy(
    name: str, num_devices: int, config: dict = None
) -> PlacementStrategy:
    """根据策略名构造策略实例。"""
    config = config or {}
    if name not in _STRATEGY_REGISTRY:
        raise ValueError(
            f"unknown strategy '{name}', available: {list_strategies()}"
        )
    return _STRATEGY_REGISTRY[name](num_devices, config)
