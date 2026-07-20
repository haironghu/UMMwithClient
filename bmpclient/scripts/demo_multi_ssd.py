#!/usr/bin/env python3
"""
演示多 SSD 设备配置与读写验证：
  - 配置 2 个 SSD 设备（客户端侧）
  - 从 SSD tier 申请多个 Chunk
  - 对每个 Chunk 进行写入/读取，验证数据一致性
  - 释放所有 Chunk

前置条件：UMM 服务已启动。
若 memory server 未配置 SSD，则演示会优雅跳过。
"""

import sys
import os

# 确保能导入 bmpclient 包
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from _common import check_services, META_ADDR, MEM_ADDR
from bmpclient.allocator import FineGrainedAllocator, MemoryExhaustedError


# 多 SSD 设备配置：2 个设备，各 64 MB
SSD_DEVICES = [
    ("/tmp/umm_ssd0.raw", 64 * 1024 * 1024),
    ("/tmp/umm_ssd1.raw", 64 * 1024 * 1024),
]


def main():
    check_services()

    print("=" * 50)
    print("  多 SSD 设备演示")
    print("=" * 50)

    print(f"\n[*] 配置 {len(SSD_DEVICES)} 个 SSD 设备:")
    for i, (path, size) in enumerate(SSD_DEVICES):
        print(f"    设备 {i}: {path} ({size // (1024 * 1024)} MB)")

    allocator = FineGrainedAllocator(
        META_ADDR, MEM_ADDR,
        default_chunk_size=2 * 1024 * 1024,
        ssd_devices=SSD_DEVICES,
    )

    print("\n=== 1. 向每个 SSD 设备申请 Chunk 并读写 ===")
    blocks = []
    try:
        # 申请 4 个 SSD Block，让它们分散到不同设备上
        for i in range(4):
            block = allocator.alloc(2 * 1024 * 1024, "ssd")
            payload = f"ssd_device_data_chunk_{i}".encode()
            # 填充到 2MB 以便验证
            payload = payload * (2 * 1024 * 1024 // len(payload))
            allocator.write(block, 0, payload)
            data = allocator.read(block, 0, len(payload))
            ok = data == payload
            print(f"  Chunk {i}: chunk_id={block.chunk_id}, size={block.size}, "
                  f"一致性={'✓ 通过' if ok else '✗ 失败'}")
            blocks.append(block)
    except MemoryExhaustedError as e:
        print(f"  ⚠ SSD 分配失败: {e}")
        print("  （可能原因：memory server 未配置 SSD，或 SSD 空间不足）")
        allocator.close()
        return

    print("\n=== 2. 再次申请 SSD Chunk（验证池仍有空间）===")
    try:
        extra = allocator.alloc(1024 * 1024, "ssd")
        allocator.write(extra, 0, b"extra_data_verify")
        data = allocator.read(extra, 0, 17)
        print(f"  额外 Chunk: chunk_id={extra.chunk_id}, "
              f"读出={data.decode()}, {'✓ 通过' if data == b'extra_data_verify' else '✗ 失败'}")
        blocks.append(extra)
    except MemoryExhaustedError as e:
        print(f"  ⚠ 额外分配失败: {e}")

    print("\n=== 3. 释放所有 SSD Chunk ===")
    for b in blocks:
        allocator.free(b)
    print(f"  ✓ 已释放 {len(blocks)} 个 SSD Chunk")

    allocator.close()
    print("\n=== 演示完成 ===")


if __name__ == "__main__":
    main()
