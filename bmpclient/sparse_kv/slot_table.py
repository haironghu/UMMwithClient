# -*- coding: utf-8 -*-
"""
bmpclient/sparse_kv/slot_table.py — 分层槽位页表（Slot Table）。

decode 定位元数据：`slot_table[layer][token_idx] -> 8B packed entry`，
扁平数组直接下标，O(1)、缓存友好、支持批量 gather。

packed entry（64 bit 无符号整数）：

    | offset : 48 bit | rsv : 8 bit | flags : 8 bit |

- offset：单元在本盘 extent 内的字节偏移（4KB 对齐，48 bit = 256TB 寻址）；
- rsv：保留，当前置 0；
- flags：仅 VALID（合并重构后介质判定 IN_BUF/ON_SSD 下沉到介质层
  `VirtualMedia.is_buffered()`，见 docs/07 §5）。

并发约定：同一 (layer, token) 整个生命周期只分配一次（单写者）；
entry 是一次性整体写入的原子 int，读路径（get/gather）无锁。

设计文档：bmpclient/docs/06_稀疏注意力KV卸载数据排布设计.md §5、
         bmpclient/docs/07_VirtualMedia合并设计_稀疏KV专用介质.md。
"""

import array
import threading
from typing import List, Tuple

FLAG_VALID = 0x01   # entry 已分配有效

_OFFSET_SHIFT = 16   # flags 8 bit + rsv 8 bit
_MAX_OFFSET = 1 << 48
_MAX_FLAGS = 1 << 8


def pack_entry(offset: int, flags: int) -> int:
    """打包 8B entry；offset 需 < 2^48，flags 为 FLAG_* 组合。"""
    if not 0 <= offset < _MAX_OFFSET:
        raise ValueError(f"offset 越出 48 bit 寻址范围: {offset}")
    if not 0 <= flags < _MAX_FLAGS:
        raise ValueError(f"flags 非法: {flags}")
    return (offset << _OFFSET_SHIFT) | flags


def entry_offset(entry: int) -> int:
    """从 packed entry 取 extent 内字节偏移。"""
    return entry >> _OFFSET_SHIFT


def entry_flags(entry: int) -> int:
    """从 packed entry 取 flags 字节。"""
    return entry & 0xFF


class SlotTable:
    """
    分层扁平数组槽位页表（单请求作用域）。

    :param num_layers: 模型层数
    :param max_tokens: 最大上下文 token 数（按此预留内存）
    """

    def __init__(self, num_layers: int, max_tokens: int):
        if num_layers <= 0 or max_tokens <= 0:
            raise ValueError("num_layers / max_tokens 必须为正")
        self._num_layers = num_layers
        self._max_tokens = max_tokens
        # array('Q')：8B 无符号定长数组，O(1) 下标
        self._layers = [
            array.array("Q", bytes(8 * max_tokens)) for _ in range(num_layers)
        ]
        # 变更（set/invalidate）串行化用；读路径（get/gather）不持锁
        self._write_lock = threading.Lock()

    @property
    def num_layers(self) -> int:
        return self._num_layers

    @property
    def max_tokens(self) -> int:
        return self._max_tokens

    def _check(self, layer_id: int, token_idx: int) -> None:
        if not 0 <= layer_id < self._num_layers:
            raise ValueError(f"layer_id 越界: {layer_id}")
        if not 0 <= token_idx < self._max_tokens:
            raise ValueError(f"token_idx 越界: {token_idx}")

    # ---- 写路径（单写者，锁内串行）----

    def set(self, layer_id: int, token_idx: int, offset: int, flags: int) -> None:
        """分配单元：写入 offset + flags（含 VALID）。"""
        self._check(layer_id, token_idx)
        with self._write_lock:
            self._layers[layer_id][token_idx] = pack_entry(offset, flags)

    def invalidate(self, layer_id: int, token_idx: int) -> None:
        """释放单元：entry 清零（VALID=0）。"""
        self._check(layer_id, token_idx)
        with self._write_lock:
            self._layers[layer_id][token_idx] = 0

    # ---- 读路径（无锁）----

    def get(self, layer_id: int, token_idx: int) -> int:
        """读取 packed entry；0 表示未分配。"""
        self._check(layer_id, token_idx)
        return self._layers[layer_id][token_idx]

    def gather(self, layer_id: int, token_indices: List[int]) -> List[int]:
        """批量读取一层内一组 token 的 packed entry（地址规划第②步）。"""
        if not 0 <= layer_id < self._num_layers:
            raise ValueError(f"layer_id 越界: {layer_id}")
        layer = self._layers[layer_id]
        return [layer[t] for t in token_indices]

    def locate(self, layer_id: int, token_idx: int) -> Tuple[int, int]:
        """便捷接口：返回 (offset, flags)；未分配返回 (0, 0)。"""
        e = self.get(layer_id, token_idx)
        return entry_offset(e), entry_flags(e)
