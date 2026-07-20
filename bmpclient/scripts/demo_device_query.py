#!/usr/bin/env python3
"""
演示新增的设备查询接口与指定设备分配：
  - 查询当前管理的 CXL / SSD 设备列表
  - 指定 device_idx 从某个 SSD 设备分配 Chunk
  - 不指定 device_idx 时保持原有行为

前置条件：UMM 服务已启动，且 memory server 配置了至少 2 个 SSD 设备。
若 memory server 未配置多 SSD，则演示会优雅跳过指定设备步骤。
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from _common import check_services, META_ADDR, MEM_ADDR
from bmpclient.allocator import FineGrainedAllocator, MemoryExhaustedError


SSD_DEVICES = [
    ("/tmp/umm_ssd0.raw", 64 * 1024 * 1024),
    ("/tmp/umm_ssd1.raw", 64 * 1024 * 1024),
]


def main():
    check_services()

    print("=" * 50)
    print("  设备查询与指定设备分配演示")
    print("=" * 50)

    allocator = FineGrainedAllocator(
        META_ADDR, MEM_ADDR,
        default_chunk_size=2 * 1024 * 1024,
        ssd_devices=SSD_DEVICES,
    )

    print("\n=== 1. 查询当前管理的设备列表 ===")
    devices = allocator.get_device_list()
    ssd_count = sum(1 for d in devices if d["tier"] == 2)
    print(f"  共 {len(devices)} 个资源，其中 SSD 设备 {ssd_count} 个")
    for i, d in enumerate(devices):
        tier_name = {0: "DRAM", 1: "CXL", 2: "SSD", 3: "RESV"}.get(d["tier"], "???")
        print(f"  [{i}] tier={tier_name} path={d['device_path']} "
              f"capacity={d['capacity'] // (1024*1024)} MB online={d['online']}")

    if ssd_count < 2:
        print("\n  ⚠ memory server 未配置多 SSD 设备，跳过指定设备分配演示")
        allocator.close()
        return

    print("\n=== 2. 不指定 device_idx（任意设备分配） ===")
    blocks = []
    try:
        for i in range(2):
            b = allocator.alloc(2 * 1024 * 1024, "ssd")
            payload = f"any_device_chunk_{i}".encode()
            payload = payload * (2 * 1024 * 1024 // len(payload))
            allocator.write(b, 0, payload)
            data = allocator.read(b, 0, len(payload))
            print(f"  Chunk {i}: chunk_id={b.chunk_id}, gpa=0x{b.gpa:016x}, "
                  f"一致性={'✓ 通过' if data == payload else '✗ 失败'}")
            blocks.append(b)
    except MemoryExhaustedError as e:
        print(f"  ⚠ 分配失败: {e}")

    print("\n=== 3. 指定 device_idx=0 分配 ===")
    try:
        b0 = allocator.alloc(2 * 1024 * 1024, "ssd", device_idx=0)
        payload0 = b"device0_specific_data" * (2 * 1024 * 1024 // 21)
        allocator.write(b0, 0, payload0)
        data0 = allocator.read(b0, 0, len(payload0))
        print(f"  Device 0 Chunk: chunk_id={b0.chunk_id}, gpa=0x{b0.gpa:016x}, "
              f"一致性={'✓ 通过' if data0 == payload0 else '✗ 失败'}")
        blocks.append(b0)
    except MemoryExhaustedError as e:
        print(f"  ⚠ 设备 0 分配失败: {e}")

    print("\n=== 4. 指定 device_idx=1 分配 ===")
    try:
        b1 = allocator.alloc(2 * 1024 * 1024, "ssd", device_idx=1)
        payload1 = b"device1_specific_data" * (2 * 1024 * 1024 // 21)
        allocator.write(b1, 0, payload1)
        data1 = allocator.read(b1, 0, len(payload1))
        print(f"  Device 1 Chunk: chunk_id={b1.chunk_id}, gpa=0x{b1.gpa:016x}, "
              f"一致性={'✓ 通过' if data1 == payload1 else '✗ 失败'}")
        blocks.append(b1)
    except MemoryExhaustedError as e:
        print(f"  ⚠ 设备 1 分配失败: {e}")

    print("\n=== 5. 释放所有 Chunk ===")
    for b in blocks:
        allocator.free(b)
    print(f"  ✓ 已释放 {len(blocks)} 个 SSD Chunk")

    allocator.close()
    print("\n=== 演示完成 ===")


if __name__ == "__main__":
    main()
