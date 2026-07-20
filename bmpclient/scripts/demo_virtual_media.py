#!/usr/bin/env python3
"""
演示 VirtualMedia 在 8 块 SSD 设备上的条带化存储：
- 启动 UMM memory service 并指定 8 块 SSD 设备
- 创建 VirtualMedia，将数据均匀分布在 8 块 SSD 上
- 通过配置文件或显式参数切换数据打散策略
- 演示 round-robin 与 consistent_hash 两种策略的写入和读取
"""

import os
import subprocess
import sys
import time

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, PROJECT_ROOT)

from bmpclient.client import UMMServiceClient
from bmpclient.virtual_media import VirtualMedia, VirtualMediaFullError


SSD_DIR = "/tmp/umm_demo_multi_ssd"
SSD_COUNT = 8
SSD_SIZE_PER_DEVICE = 16 * 1024 * 1024  # 16 MB per SSD
VM_SIZE = SSD_COUNT * 8 * 1024 * 1024   # 64 MB total, 8 MB per device
GRANULARITY = 1024 * 1024               # 1 MB per record
SLOTS_PER_DEVICE = (VM_SIZE // SSD_COUNT) // GRANULARITY  # 8 slots per device


def start_umm_services():
    """启动 UMM 服务，指定 8 块 SSD 设备。"""
    os.makedirs(SSD_DIR, exist_ok=True)

    devices = ",".join(
        f"{SSD_DIR}/ssd{i:02d}.raw:{SSD_SIZE_PER_DEVICE // (1024 * 1024)}M"
        for i in range(SSD_COUNT)
    )

    start_script = os.path.join(PROJECT_ROOT, "UMM", "scripts", "start_services.sh")
    cmd = [
        start_script,
        "-s", "128M",
        "-D", devices,
    ]

    print(f"启动 UMM 服务（{SSD_COUNT} 块 SSD）...")
    print(f"  SSD 设备: {devices}")
    subprocess.run(cmd, check=True)

    # 等待端口就绪
    for _ in range(20):
        try:
            import socket
            with socket.create_connection(("127.0.0.1", 20001), timeout=0.5):
                with socket.create_connection(("127.0.0.1", 20002), timeout=0.5):
                    break
        except Exception:
            time.sleep(0.2)
    else:
        raise RuntimeError("UMM services failed to become ready")

    print("  UMM 服务已就绪\n")


def stop_umm_services():
    """停止 UMM 服务。"""
    stop_script = os.path.join(PROJECT_ROOT, "UMM", "scripts", "stop_services.sh")
    subprocess.run([stop_script], check=False)
    print("UMM 服务已停止")


def create_client():
    """创建 UMMServiceClient，配置 8 块 SSD 设备。"""
    ssd_devices = [
        (f"{SSD_DIR}/ssd{i:02d}.raw", SSD_SIZE_PER_DEVICE)
        for i in range(SSD_COUNT)
    ]
    return UMMServiceClient(
        "127.0.0.1:20001",
        "127.0.0.1:20002",
        node_id=0,
        ssd_devices=ssd_devices,
    )


def run_strategy_demo(client, strategy_name, write_count=None):
    """使用指定策略创建 VirtualMedia 并验证读写。"""
    print(f"\n=== 策略: {strategy_name} ===")
    vm = VirtualMedia(client.lib, size=VM_SIZE, granularity=GRANULARITY, strategy=strategy_name)
    print(f"创建 VirtualMedia: size={VM_SIZE}, granularity={GRANULARITY}")
    print(f"  当前策略: {vm.strategy.name}")
    print(f"  设备数: {vm.device_count}")
    print(f"  槽位数: {vm.slot_count}")
    print(f"  每设备槽位数: {SLOTS_PER_DEVICE}")

    # 默认写满；一致性哈希分布不均，避免溢出只写入少量数据做验证
    if write_count is None:
        write_count = vm.slot_count

    # 顺序写入
    print(f"\n顺序写入 {write_count} 条数据：")
    records = []
    for i in range(write_count):
        data = f"record-{i:04d}-{strategy_name}".encode().ljust(GRANULARITY, b'\0')
        idx = vm.save(data)
        records.append((idx, data))

    # 统计每块 SSD 上的条目数
    counts = {d: 0 for d in range(SSD_COUNT)}
    for idx, _ in records:
        device_idx = vm.strategy.locate(idx)
        counts[device_idx] += 1

    print("\n每块 SSD 设备上的条目数统计：")
    for d in range(SSD_COUNT):
        print(f"  device {d}: {counts[d]} 条")

    # round-robin 期望均匀分布
    if strategy_name == "round_robin":
        for d in range(SSD_COUNT):
            assert counts[d] == SLOTS_PER_DEVICE, f"device {d} 条目数异常"
        print("  round-robin 分布均匀，验证通过")

    # 按索引读取并验证
    print("\n读取验证（抽样）：")
    sample_indices = [0, 1, min(7, write_count - 1)]
    if write_count > 8:
        sample_indices.append(min(8, write_count - 1))
    if write_count > 16:
        sample_indices.extend([15, 16, 31, min(63, write_count - 1)])
    sample_indices = sorted(set(i for i in sample_indices if i < write_count))
    for idx in sample_indices:
        read_data = vm.read(idx)
        expected = records[idx][1]
        text = read_data.rstrip(b'\0').decode("utf-8")
        assert read_data == expected, f"slot {idx} 数据不一致"
        print(f"  read({idx:2d}) = {text}")

    # 验证写满异常（仅在写满时）
    if write_count == vm.slot_count:
        print("\n尝试写满后再写入：")
        try:
            vm.save(b"overflow" * (GRANULARITY // 8))
        except VirtualMediaFullError as e:
            print(f"  预期异常: {e}")

    vm.close()
    print("\nVirtualMedia 已关闭，底层 SSD Chunk 已释放。")
    return counts


def main():
    # 若已有服务在运行，先停止以避免冲突
    stop_umm_services()
    time.sleep(0.5)

    start_umm_services()

    try:
        client = create_client()
        try:
            # 演示 1：默认配置文件策略（当前默认为 round_robin）
            run_strategy_demo(client, "round_robin")

            # 演示 2：一致性哈希策略（写入 8 条，避免非均匀分布导致单设备溢出）
            run_strategy_demo(client, "consistent_hash", write_count=8)

            print("\n所有策略演示完成。")
        finally:
            client.close()
    finally:
        stop_umm_services()


if __name__ == "__main__":
    main()
