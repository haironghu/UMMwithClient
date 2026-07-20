#!/usr/bin/env python3
"""
演示 FineGrainedAllocator 的基本功能：
  - alloc / free / read / write
  - 批量预分配
  - DRAM / SSD 隔离
  - Chunk 自动回收

前置条件：UMM 服务已启动。
"""

import sys
import os

# 确保能导入 bmpclient 包
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from _common import check_services, META_ADDR, MEM_ADDR
from bmpclient.allocator import FineGrainedAllocator, MemoryExhaustedError
from bmpclient.umm_client import ChunkDescriptor


SSD_MOCK_DEVICE = "/tmp/umm_mock_ssd.raw"

# 多 SSD 设备配置示例（新版 API）
SSD_DEVICES = [
    ("/tmp/umm_ssd0.raw", 64 * 1024 * 1024),   # 64 MB
    ("/tmp/umm_ssd1.raw", 128 * 1024 * 1024),  # 128 MB
]


def main():
    check_services()

    # 优先尝试多设备配置；若 memory server 未配置 SSD，则 gracefully 降级
    allocator = FineGrainedAllocator(
        META_ADDR, MEM_ADDR,
        default_chunk_size=2 * 1024 * 1024,
        ssd_device=SSD_MOCK_DEVICE,
        # ssd_devices=SSD_DEVICES,  # 取消注释以启用多设备
    )

    print("\n=== 1. 基本 alloc / free ===")
    block = allocator.alloc(1024, "dram")
    print(f"  分配 Block: chunk_id={block.chunk_id}, offset={block.offset}, size={block.size}")
    allocator.free(block)
    print("  ✓ 已释放")

    print("\n=== 2. 同一 Chunk 内多次分配 ===")
    b1 = allocator.alloc(1024, "dram")
    b2 = allocator.alloc(1024, "dram")
    print(f"  b1: chunk={b1.chunk_id}, offset={b1.offset}")
    print(f"  b2: chunk={b2.chunk_id}, offset={b2.offset}")
    print(f"  是否同一 Chunk: {'是' if b1.chunk_id == b2.chunk_id else '否'}")
    allocator.free(b1)
    allocator.free(b2)

    print("\n=== 3. Block 级 read / write (DRAM) ===")
    block = allocator.alloc(4096, "dram")
    payload = b"hello umm read/write"
    allocator.write(block, 0, payload)
    data = allocator.read(block, 0, len(payload))
    print(f"  写入: {payload.decode()}")
    print(f"  读出: {data.decode()}")
    print(f"  一致性: {'✓ 通过' if data == payload else '✗ 失败'}")

    # 带偏移的读写
    allocator.write(block, 1024, b"offset data")
    offset_data = allocator.read(block, 1024, 11)
    print(f"  偏移 1024 写入后读出: {offset_data.decode()}")
    allocator.free(block)

    print("\n=== 4. Block 级 read / write (SSD) ===")
    try:
        ssd_block = allocator.alloc(4096, "ssd")
        ssd_payload = b"ssd block demo"
        allocator.write(ssd_block, 0, ssd_payload)
        ssd_data = allocator.read(ssd_block, 0, len(ssd_payload))
        print(f"  写入: {ssd_payload.decode()}")
        print(f"  读出: {ssd_data.decode()}")
        print(f"  一致性: {'✓ 通过' if ssd_data == ssd_payload else '✗ 失败'}")
        allocator.free(ssd_block)
    except MemoryExhaustedError as e:
        print(f"  ⚠ 跳过: memory server 未配置 SSD ({e})")

    print("\n=== 5. 批量预分配 ===")
    allocator.preallocate_chunks(2, 2 * 1024 * 1024, "dram")
    print(f"  预分配后 DRAM Chunk 数量: {len(allocator.chunks_by_type['dram'])}")
    # 清理预分配的 Chunk
    for chunk_id in list(allocator.chunks_by_type["dram"].keys()):
        cb = allocator.chunks_by_type["dram"][chunk_id]
        desc = ChunkDescriptor(
            chunk_id=cb.chunk_id,
            base_gpa=cb.base_gpa,
            user_size=cb.total_size,
        )
        allocator.client.delete_chunk(desc)
    allocator.chunks_by_type["dram"].clear()

    print("\n=== 6. DRAM / SSD 隔离 ===")
    dram_blocks = []
    # memory server 配置 64M，每个 chunk 2M，分配 32 个可耗尽 DRAM
    for i in range(32):
        try:
            b = allocator.alloc(2 * 1024 * 1024, "dram")
            dram_blocks.append(b)
        except MemoryExhaustedError:
            print(f"  已分配 {len(dram_blocks)} 个 DRAM Block 后耗尽")
            break
    else:
        print(f"  已分配 {len(dram_blocks)} 个 DRAM Block")

    try:
        allocator.alloc(1024, "dram")
        print("  ✗ 错误: DRAM 应该已耗尽")
    except MemoryExhaustedError:
        print("  ✓ DRAM 已耗尽，无法再分配")

    try:
        ssd_block = allocator.alloc(1024, "ssd")
        print(f"  ✓ SSD 仍可分配: chunk={ssd_block.chunk_id}")
        allocator.free(ssd_block)
    except MemoryExhaustedError as e:
        print(f"  ⚠ 跳过: memory server 未配置 SSD ({e})")
    finally:
        # 释放所有 DRAM Block
        for b in dram_blocks:
            allocator.free(b)
        print(f"  ✓ 已释放 {len(dram_blocks)} 个 DRAM Block")

    print("\n=== 7. Chunk 自动回收 ===")
    block = allocator.alloc(1024 * 1024, "dram")
    chunk_id = block.chunk_id
    print(f"  分配 Chunk: {chunk_id}")
    allocator.free(block)
    if chunk_id not in allocator.chunks_by_type["dram"]:
        print("  ✓ Chunk 已自动回收")
    else:
        print("  ✗ Chunk 未被回收")

    allocator.close()
    print("\n=== 演示完成 ===")


if __name__ == "__main__":
    main()
