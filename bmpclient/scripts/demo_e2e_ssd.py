#!/usr/bin/env python3
"""
demo_e2e_ssd.py — Phase 3 场景 D：真实 SSD tier 端到端。

场景流程：
  1. 生成 umms 配置（SSD tier 设备来自 --spec）
  2. 启动 umm-metadata-service（ummd）与 umm-memory-server（umms -c 配置）
  3. client 以 tier_aware 模式连接（RPC 分配面 + 本地 tier 路由数据面）
  4. client.enable_ssd()：上报 ummD 拓扑 + 建立本地 SSD 数据面
  5. D0：拓扑校验（get_device_list 应看到 SSD 设备）
  6. D1：基础读写校验（create_chunk(ssd) → pattern 写 → 读回 memcmp）
  7. D2：ConcurrentIOEngine 并发批量读写（2048 条 4KB，4 chunk 布局），
     逐条数据校验 + 串行基线耗时对照
  8. 关闭服务，清理临时文件

数据面事实（RPC 模式）：
  分配面走 RPC（umms 的 ssd_pool 是分配权威）；数据面在 client 进程本地
  （tier_router → transport_ssd → 本地 ssd_pool → 设备）。
  文件后端走 mmap+memcpy；libnvm 后端 map_device 失败自动回退
  ssd_read/ssd_write 主机通路。两端打开同一设备，容量必须一致。

用法：
  # 文件后端（CI / 无盘环境，默认）
  UMM_ROOT=/path/to/umm python3 scripts/demo_e2e_ssd.py

  # 真机 libnvm（窗口 [1GB, 1GB+16GB)）
  export UMM_LIBNVM_PATH=/home/l00835206/code/NPU_Direct_Storage/build/lib/libnvm_host.so
  export LD_LIBRARY_PATH=<传递依赖目录>:$LD_LIBRARY_PATH
  python3 scripts/demo_e2e_ssd.py \
      --spec "libnvm:/dev/libnvm_helper0@1+0x40000000" --capacity 16G

环境变量（均可被命令行覆盖）：
  UMM_SSD_SPEC      设备路径（缺省 /tmp/umm_e2e_ssd.raw）
  UMM_SSD_CAPACITY  容量（缺省 256M；后缀 G/M/K 或纯字节数）
"""

import argparse
import os
import random
import socket
import subprocess
import sys
import tempfile
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PKG_DIR = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.dirname(PKG_DIR))

META_PORT = 20001
MEM_PORT = 20002
MEM_SIZE = 128 * 1024 * 1024
NUM_CHUNKS = 4
PER_CHUNK = 8 * 1024 * 1024
NUM_IOS = 2048
IO_SIZE = 4096


def parse_size(s: str) -> int:
    s = s.strip()
    mult = {"G": 1 << 30, "M": 1 << 20, "K": 1 << 10}
    if s and s[-1].upper() in mult:
        return int(s[:-1]) * mult[s[-1].upper()]
    return int(s, 0)


def find_umm_root() -> str:
    candidates = []
    env = os.environ.get("UMM_ROOT")
    if env:
        candidates.append(env)
    candidates += [
        os.path.join(PKG_DIR, "..", "umm"),
        os.path.join(PKG_DIR, "..", "UMM"),
        os.path.join(PKG_DIR, "..", "..", "umm"),
    ]
    for path in candidates:
        if os.path.isdir(os.path.join(path, "bin")):
            return os.path.abspath(path)
    raise FileNotFoundError(f"找不到 UMM 根目录，尝试过: {candidates}")


def find_server_binary(umm_root: str, names) -> str:
    for name in names:
        path = os.path.join(umm_root, "bin", name)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    raise FileNotFoundError(f"在 {umm_root}/bin 下找不到任一: {names}")


def wait_port(host: str, port: int, timeout: float = 8.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def write_umms_config(spec: str, capacity: int) -> str:
    text = (
        f"node_id: 0\n"
        f'listen_addr: "127.0.0.1"\n'
        f"listen_port: {MEM_PORT}\n"
        f"memory_size: {MEM_SIZE}\n"
        f"base_gpa: 0\n"
        f'ssd_devices: "{spec}:{capacity}"\n'
    )
    fd, path = tempfile.mkstemp(prefix="umm_e2e_umms_", suffix=".yaml")
    with os.fdopen(fd, "w") as f:
        f.write(text)
    return path


def start_servers(umm_root: str, cfg_path: str):
    ummd_bin = find_server_binary(umm_root, ["umm-metadata-service", "ummd"])
    umms_bin = find_server_binary(umm_root, ["umm-memory-server", "umms"])
    print(f"[servers] umm-metadata-service: {ummd_bin}")
    print(f"[servers] umm-memory-server   : {umms_bin} -c {cfg_path}")

    proc_ummd = subprocess.Popen(
        [ummd_bin, "-p", str(META_PORT), "-b", "127.0.0.1"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    proc_umms = subprocess.Popen(
        [umms_bin, "-c", cfg_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if not wait_port("127.0.0.1", META_PORT):
        raise RuntimeError("umm-metadata-service 端口未就绪")
    if not wait_port("127.0.0.1", MEM_PORT):
        raise RuntimeError("umm-memory-server 端口未就绪（SSD 设备打开失败？）")
    print(f"[servers] ummd:{META_PORT} umms:{MEM_PORT} 已就绪")
    return proc_ummd, proc_umms


def stop_servers(*procs):
    for p in procs:
        if p and p.poll() is None:
            p.terminate()
    for p in procs:
        if p:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
    print("[servers] 已关闭")


def scenario_topology(client, spec: str, capacity: int):
    """D0：拓扑校验 —— ummD 应能返回本节点注册的 SSD 资源。"""
    print("\n=== D0：拓扑校验（get_device_list）===")
    devs = client.get_device_list()
    ssd = [d for d in devs if d["tier"] == 2]
    assert ssd, f"拓扑中未找到 SSD 资源: {devs}"
    d = ssd[0]
    print(f"[D0] SSD 资源: path={d['device_path']} capacity={d['capacity']}")
    assert d["device_path"] == spec, (
        f"设备路径不一致: topo={d['device_path']} spec={spec}")
    assert d["capacity"] == capacity, (
        f"容量不一致: topo={d['capacity']} spec={capacity}")
    print("[D0] PASS：设备路径与容量和注册值一致")


def scenario_basic_rw(client):
    """D1：SSD chunk 基础读写校验。"""
    print("\n=== D1：SSD chunk 基础读写 ===")
    desc = client.create_chunk(1 * 1024 * 1024, "ssd")
    print(f"[D1] ssd chunk 已分配: base_gpa=0x{desc.base_gpa:x}")

    pattern = bytes((i * 7 + 13) & 0xFF for i in range(65536))
    client.write_chunk(desc, 0, pattern)
    client.write_chunk(desc, 65536, pattern)
    back1 = client.read_chunk(desc, 0, 65536)
    back2 = client.read_chunk(desc, 65536, 65536)
    assert back1 == pattern and back2 == pattern, "数据校验失败"
    print("[D1] 2 段 64KB 写入/读回校验 PASS")
    client.delete_chunk(desc)


def scenario_concurrent_io(client):
    """D2：引擎并发批量读写 + 串行基线对照（并行粒度=chunk）。"""
    print("\n=== D2：并发批量读写（ConcurrentIOEngine → SSD tier）===")
    from bmpclient.concurrent_io import (
        ConcurrentIOEngine, IOAddress, IORequest,
    )

    descs = [client.create_chunk(PER_CHUNK, "ssd") for _ in range(NUM_CHUNKS)]
    print(f"[D2] 已分配 {NUM_CHUNKS} 个 ssd chunk x "
          f"{PER_CHUNK // 1024 // 1024}MB")

    addrs = [(descs[i % NUM_CHUNKS], (i // NUM_CHUNKS) * IO_SIZE)
             for i in range(NUM_IOS)]
    random.seed(42)
    random.shuffle(addrs)

    def make_payload(seq: int) -> bytes:
        return seq.to_bytes(4, "little") + bytes(
            (seq + i) & 0xFF for i in range(IO_SIZE - 4))

    write_buf = bytearray(NUM_IOS * IO_SIZE)
    for i in range(NUM_IOS):
        write_buf[i * IO_SIZE:(i + 1) * IO_SIZE] = make_payload(i)

    engine = ConcurrentIOEngine(client.lib, num_workers=8, num_queues=4)
    reqs = [
        IORequest(
            IOAddress.from_descriptor(addrs[i][0], addrs[i][1], IO_SIZE),
            memoryview(write_buf)[i * IO_SIZE:(i + 1) * IO_SIZE],
        )
        for i in range(NUM_IOS)
    ]
    t0 = time.perf_counter()
    engine.submit_write(reqs).result()
    write_ms = (time.perf_counter() - t0) * 1000
    print(f"[D2] 并发写 {NUM_IOS}x{IO_SIZE}B: {write_ms:.2f}ms "
          f"({NUM_IOS * IO_SIZE / write_ms / 1000:.1f} MB/s)")

    read_buf = bytearray(NUM_IOS * IO_SIZE)
    rreqs = [
        IORequest(
            IOAddress.from_descriptor(addrs[i][0], addrs[i][1], IO_SIZE),
            memoryview(read_buf)[i * IO_SIZE:(i + 1) * IO_SIZE],
        )
        for i in range(NUM_IOS)
    ]
    t0 = time.perf_counter()
    engine.read_batch(rreqs)
    read_ms = (time.perf_counter() - t0) * 1000

    bad = sum(
        1 for i in range(NUM_IOS)
        if read_buf[i * IO_SIZE:(i + 1) * IO_SIZE] != make_payload(i))
    assert bad == 0, f"{bad} 条数据校验失败"
    print(f"[D2] 并发读 {NUM_IOS}x{IO_SIZE}B: {read_ms:.2f}ms, "
          f"{NUM_IOS} 条全部校验 PASS")

    t0 = time.perf_counter()
    for i in range(NUM_IOS):
        client.read_chunk(addrs[i][0], addrs[i][1], IO_SIZE)
    serial_ms = (time.perf_counter() - t0) * 1000

    stats = engine.stats()
    engine.close()
    for d in descs:
        client.delete_chunk(d)

    print(f"[D2] 串行读基线: {serial_ms:.2f}ms | 并发读: {read_ms:.2f}ms "
          f"| 加速 {serial_ms / read_ms:.2f}x")
    print(f"[D2] 引擎统计: submitted={stats.get('submitted')} "
          f"completed={stats.get('completed')} failed={stats.get('failed')}")
    assert stats.get("failed") == 0


def main():
    parser = argparse.ArgumentParser(description="Phase 3 场景 D：真实 SSD tier e2e")
    parser.add_argument(
        "--spec",
        default=os.environ.get("UMM_SSD_SPEC", "/tmp/umm_e2e_ssd.raw"),
        help='SSD 设备路径（文件 或 "libnvm:<ctrl>[@ns][+<base_off>]"）',
    )
    parser.add_argument(
        "--capacity",
        default=os.environ.get("UMM_SSD_CAPACITY", "256M"),
        help="SSD 窗口/文件容量（后缀 G/M/K 或字节数，缺省 256M）",
    )
    args = parser.parse_args()

    spec = args.spec
    capacity = parse_size(args.capacity)
    is_libnvm = spec.startswith("libnvm:")

    if is_libnvm and not os.environ.get("UMM_LIBNVM_PATH"):
        print("错误: 使用 libnvm 设备必须先 export UMM_LIBNVM_PATH="
              "/path/to/libnvm_host.so（及 LD_LIBRARY_PATH 传递依赖）")
        sys.exit(2)

    print(f"[setup] SSD spec={spec}")
    print(f"[setup] 容量={capacity} ({capacity / 1024 / 1024:.0f}MB) "
          f"后端={'libnvm' if is_libnvm else '文件模拟'}")

    umm_root = find_umm_root()
    os.environ.setdefault("UMM_BUILD_DIR", os.path.join(umm_root, "build"))
    print(f"[setup] UMM_ROOT={umm_root}")

    if not is_libnvm and os.path.exists(spec):
        os.unlink(spec)

    cfg_path = write_umms_config(spec, capacity)
    procs = start_servers(umm_root, cfg_path)
    try:
        from bmpclient.client import UMMServiceClient
        client = UMMServiceClient(
            meta_addr=f"127.0.0.1:{META_PORT}",
            mem_addr=f"127.0.0.1:{MEM_PORT}",
            node_id=0,
            tier_aware=True,
        )
        print("[client] 已连接（tier_aware：RPC 分配面 + 本地 tier 路由数据面）")

        client.enable_ssd(spec, capacity)
        print("[client] SSD tier 已注册（ummD 拓扑 + 本地数据面）")

        scenario_topology(client, spec, capacity)
        scenario_basic_rw(client)
        scenario_concurrent_io(client)

        client.close()
    finally:
        stop_servers(*procs)
        os.unlink(cfg_path)
        if not is_libnvm and os.path.exists(spec):
            os.unlink(spec)

    print("\n*** 场景 D 全部 PASS ***")


if __name__ == "__main__":
    main()
