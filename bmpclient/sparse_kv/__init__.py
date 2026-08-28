# -*- coding: utf-8 -*-
"""
bmpclient/sparse_kv/ — 稀疏注意力 KV cache 卸载框架对接层。

合并重构后（docs/07）：
- VirtualMedia（介质层）负责盘感知、打散策略与段聚合写；
- SparseKVStore 负责 vllm block 语义、slot_table 元数据与地址规划 plan()；
- 读路径不执行 IO，只向 GPU 直通算子输出地址映射表。

设计文档：bmpclient/docs/06_稀疏注意力KV卸载数据排布设计.md、
         bmpclient/docs/07_VirtualMedia合并设计_稀疏KV专用介质.md。
"""

from bmpclient.sparse_kv.slot_table import (
    FLAG_VALID,
    SlotTable,
    entry_flags,
    entry_offset,
    pack_entry,
)
from bmpclient.sparse_kv.store import PlanView, SparseKVStore
from bmpclient.sparse_kv.vllm_adapter import KVBlockRef
from bmpclient.virtual_media import SegmentFullError

__all__ = [
    "SparseKVStore",
    "PlanView",
    "KVBlockRef",
    "SlotTable",
    "FLAG_VALID",
    "pack_entry",
    "entry_offset",
    "entry_flags",
    "SegmentFullError",
]
