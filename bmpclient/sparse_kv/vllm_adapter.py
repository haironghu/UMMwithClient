# -*- coding: utf-8 -*-
"""
bmpclient/sparse_kv/vllm_adapter.py — vllm KV block 信息适配。

KVBlockRef 是 offload 的入参单元：vllm 以 block（block_size 个 token）
管理 KV cache，本模块仅定义数据载体；block 在 SparseKVMedia.offload 中
逐 token 展开为排布单元（buffer 按 token 主序连续排布，展开只是
memoryview 切片，零拷贝）。

设计文档：bmpclient/docs/06_稀疏注意力KV卸载数据排布设计.md §6。
"""

from dataclasses import dataclass


@dataclass
class KVBlockRef:
    """vllm 管理的 KV block 信息（CPU 侧）。"""

    layer_id: int           # 模型层编号
    block_idx: int          # vllm block table 中的块号（透传，不参与排布）
    token_start: int        # block 内首个 token 的全局 token_idx
    token_count: int        # 本 block 的 token 数（通常 = vllm block_size）
    buffer: memoryview      # K/V 数据，token 主序连续：token i 在
                            # buffer[i*unit_size : (i+1)*unit_size]

    def __post_init__(self):
        if not isinstance(self.buffer, memoryview):
            self.buffer = memoryview(self.buffer)
