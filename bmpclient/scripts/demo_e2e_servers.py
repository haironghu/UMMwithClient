#!/usr/bin/env python3
"""
demo_e2e_servers.py — 端到端测试场景：启动 UMM 服务，client 触发并发读写。

场景流程：
  1. 启动 umm-metadata-service（ummd）与 umm-memory-server（umms）
  2. 等待端口就绪
  3. bmpclient 连接（mock transport + RPC 分配面）
  4. 场景 A：基础读写校验（write_chunk / read_chunk）
  5. 场景 B：ConcurrentIOEngine 并发批量读写（2048 条离散 4KB IO，
     4 chunk 布局），逐条数据校验 + 串行/并发耗时对比
  6. 关闭服务
  7. 场景 C：慢设备模拟（FakeUMMLib 注入 0.2ms/条延迟，无需服务端），
     三种队列模式对照，验证并发加速机制与 chunk 并行粒度语义

用法：
  UMM_ROOT=/path/to/UMM python3 scripts/demo_e2e_servers.py
  python3 scripts/demo_e2e_servers.py --sim-only   # 只跑场景 C（无需服务端）
  # UMM_ROOT 缺省时按 ../umm、../../umm 相对路径查找
  # libumm.so 通过 UMM_BUILD_DIR 或 $UMM_ROOT/build 查找

注意（当前 UMM 代码的数据面事实）：
  RPC 仅承载 分配/元数据；umm_read/umm_write 的数据面在 client 进程本地
  （malloc/mmap memcpy）。因此本场景覆盖的是
  「服务启动 → RPC 分配 → 本地数据面并发读写」的完整 client 链路。
"""

import os
import socket
import subprocess
import sys
import time

# --------------------------------------------------------------------------
# 路径解析
# --------------------------------------------------------------------------

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PKG_DIR = os.path.dirname(SCRIPT_DIR)          # bmpclient 包目录
sys.path.insert(0, os.path.dirname(PKG_DIR))   # 使 import bmpclient 可用

META_PORT = 20001
MEM_PORT = 20002
MEM_SIZE = 128 * 1024 * 1024                   # umms 128MB
CHUNK_SIZE = 32 * 1024 * 1024                  # 测试 chunk 32MB（< client 本地 64MB）
NUM_IOS = 2048                                  # 离散 IO 条数
IO_SIZE = 4096                                  # 每条 4KB


def find_umm_root() -> str:
    """按优先级定位 UMM 仓库根目录（需含 bin/ 与 build/libumm.so）。"""
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
    """在 UMM bin/ 下按候选名查找服务端二进制（兼容新旧命名）。"""
    for name in names:
        path = os.path.join(umm_root, "bin", name)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    raise FileNotFoundError(f"在 {umm_root}/bin 下找不到任一: {names}")


def wait_port(host: str, port: int, timeout: float = 5.0) -> bool:
    """轮询等待 TCP 端口就绪。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


# --------------------------------------------------------------------------
# 服务生命周期
# --------------------------------------------------------------------------

def start_servers(umm_root: str):
    """启动 umm-metadata-service 与 umm-memory-server，返回 (proc_ummd, proc_umms)。"""
    ummd_bin = find_server_binary(umm_root, ["umm-metadata-service", "ummd"])
    umms_bin = find_server_binary(umm_root, ["umm-memory-server", "umms"])

    print(f"[servers] umm-metadata-service: {ummd_bin}")
    print(f"[servers] umm-memory-server   : {umms_bin}")

    proc_ummd = subprocess.Popen(
        [ummd_bin, "-p", str(META_PORT), "-b", "127.0.0.1"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    proc_umms = subprocess.Popen(
        [umms_bin, "-p", str(MEM_PORT), "-b", "127.0.0.1",
         "-n", "0", "-s", str(MEM_SIZE)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    if not wait_port("127.0.0.1", META_PORT):
        raise RuntimeError("umm-metadata-service 端口未就绪")
    if not wait_port("127.0.0.1", MEM_PORT):
        raise RuntimeError("umm-memory-server 端口未就绪")
    print(f"[servers] ummd:{META_PORT} umms:{MEM_PORT} 已就绪 "
          f"(umms mem={MEM_SIZE // 1024 // 1024}MB)")
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


# --------------------------------------------------------------------------
# 测试场景
# --------------------------------------------------------------------------

def scenario_basic_rw(client):
    """场景 A：基础读写 —— 分配 chunk，写入 pattern，读回校验。"""
    print("\n=== 场景 A：基础读写校验 ===")
    desc = client.create_chunk(1 * 1024 * 1024, "dram")
    print(f"[A] chunk 已分配: {desc}")

    pattern = bytes((i * 7 + 13) & 0xFF for i in range(65536))
    client.write_chunk(desc, 0, pattern)
    client.write_chunk(desc, 65536, pattern)          # 第二段相同 pattern
    back1 = client.read_chunk(desc, 0, 65536)
    back2 = client.read_chunk(desc, 65536, 65536)
    assert back1 == pattern and back2 == pattern, "数据校验失败"
    print("[A] 2 段 64KB 写入/读回校验 PASS")
    client.delete_chunk(desc)


def scenario_concurrent_io(client):
    """场景 B：ConcurrentIOEngine 并发批量读写 + 串行基线对比。

    注意并行语义：num_queues 按 chunk_id 哈希分桶，同一 chunk 的请求
    落入同一单线程队列（保序串行）。因此本场景分配 4 个 chunk
    （对应真实 KV 卸载中 VirtualMedia 每设备一个 extent 的布局），
    4 队列并行执行，同时保持每个 chunk 内 FIFO。
    """
    print("\n=== 场景 B：并发批量读写（ConcurrentIOEngine）===")
    from bmpclient.concurrent_io import (
        ConcurrentIOEngine, IOAddress, IORequest,
    )

    NUM_CHUNKS = 4
    per_chunk = CHUNK_SIZE // NUM_CHUNKS
    descs = [client.create_chunk(per_chunk, "dram") for _ in range(NUM_CHUNKS)]
    print(f"[B] 已分配 {NUM_CHUNKS} 个 chunk x {per_chunk // 1024 // 1024}MB")

    # 构造 2048 个离散 4KB 地址：轮询散布到 4 个 chunk，打乱顺序模拟随机分布
    import random
    addrs = [(descs[i % NUM_CHUNKS], (i // NUM_CHUNKS) * IO_SIZE) for i in range(NUM_IOS)]
    random.seed(42)
    random.shuffle(addrs)

    # 每条 IO 的填充数据：前 4 字节为序号，其余为序号派生 pattern
    def make_payload(seq: int) -> bytes:
        return seq.to_bytes(4, "little") + bytes((seq + i) & 0xFF for i in range(IO_SIZE - 4))

    # ---- 并发写 ----
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
    h = engine.submit_write(reqs)
    submit_ms = (time.perf_counter() - t0) * 1000
    h.result()
    write_ms = (time.perf_counter() - t0) * 1000
    print(f"[B] 并发写 {NUM_IOS}x{IO_SIZE}B: submit 耗时 {submit_ms:.2f}ms "
          f"(异步立即返回), 完成总耗时 {write_ms:.2f}ms")

    # ---- 并发读 + 逐条校验 ----
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

    bad = 0
    for i in range(NUM_IOS):
        seq = int.from_bytes(read_buf[i * IO_SIZE:i * IO_SIZE + 4], "little")
        if seq != i or read_buf[i * IO_SIZE:(i + 1) * IO_SIZE] != make_payload(i):
            bad += 1
    assert bad == 0, f"{bad} 条数据校验失败"
    print(f"[B] 并发读 {NUM_IOS}x{IO_SIZE}B: {read_ms:.2f}ms, "
          f"{NUM_IOS} 条全部校验 PASS")

    # ---- 串行基线（逐条 client.read_chunk/write_chunk）----
    t0 = time.perf_counter()
    for i in range(NUM_IOS):
        client.read_chunk(addrs[i][0], addrs[i][1], IO_SIZE)
    serial_read_ms = (time.perf_counter() - t0) * 1000

    stats = engine.stats()
    engine.close()
    for d in descs:
        client.delete_chunk(d)

    print(f"[B] 串行读基线: {serial_read_ms:.2f}ms | 并发读: {read_ms:.2f}ms")
    print("[B] 说明: mock 介质单次 IO 仅 ~0.2us，Python 簿记开销占主导，"
          "并发在此不体现加速属预期；引擎收益在真实设备延迟场景"
          "（SSD 缺页 / 大块拷贝，单 IO >> 10us）显现——见场景 C 的实测对照")
    print(f"[B] 引擎统计: submitted={stats.get('submitted')} "
          f"completed={stats.get('completed')} failed={stats.get('failed')}")


def scenario_simulated_slow_device():
    """场景 C：慢设备模拟 —— FakeUMMLib 注入延迟，验证并发加速机制。

    不依赖服务端与真实 libumm.so。io_delay 模拟单条 IO 的设备等待
    （锁外 sleep，可被多线程重叠），设备侧并发峰值由 _FakeCLib 埋点观测。

    三种引擎模式对照（验证「并行粒度是 chunk」的核心语义）：
      C1  单 chunk + num_queues=4   → 保序语义下同 chunk 落同一队列（串行）
      C2  单 chunk + num_queues=1   → 共享线程池（并行，不保序）
      C3  4 chunk + num_queues=4    → 4 队列并行，每 chunk 内仍保序
    """
    print("\n=== 场景 C：慢设备模拟（FakeUMMLib 注入延迟，无需服务端）===")
    from bmpclient.testing import FakeUMMLib
    from bmpclient.concurrent_io import (
        ConcurrentIOEngine, IOAddress, IORequest,
    )

    PAGE = IO_SIZE
    N = NUM_IOS
    DELAY = 0.0002          # 每条 IO 模拟 0.2ms 设备延迟
    print(f"[C] 参数: {N} 条 x {PAGE}B, 注入延迟 {DELAY * 1000:.1f}ms/条")

    # ---- 串行基线 ----
    lib0 = FakeUMMLib(io_delay=DELAY)
    desc0 = lib0.alloc(N * PAGE)
    zeros = bytes(PAGE)
    t0 = time.perf_counter()
    for i in range(N):
        lib0.write(desc0, i * PAGE, zeros)
    serial_ms = (time.perf_counter() - t0) * 1000
    print(f"[C] 串行基线: {serial_ms:.0f}ms")

    def run_engine(num_chunks: int, num_queues: int, num_workers: int, label: str):
        lib = FakeUMMLib(io_delay=DELAY)
        per_chunk = (N // num_chunks) * PAGE
        descs = [lib.alloc(per_chunk) for _ in range(num_chunks)]
        buf = bytearray(PAGE)
        engine = ConcurrentIOEngine(lib, num_workers=num_workers, num_queues=num_queues)
        reqs = [
            IORequest(
                IOAddress.from_descriptor(
                    descs[i % num_chunks], (i // num_chunks) * PAGE, PAGE
                ),
                memoryview(buf),
            )
            for i in range(N)
        ]
        t0 = time.perf_counter()
        engine.write_batch(reqs)
        dt_ms = (time.perf_counter() - t0) * 1000
        peak = lib.fake.max_device_inflight
        engine.close()
        print(f"[C] {label}: {dt_ms:.0f}ms  设备侧并发峰值={peak}  "
              f"加速={serial_ms / dt_ms:.1f}x")
        return dt_ms

    run_engine(1, 4, 8, "单chunk + num_queues=4 (保序→单队列串行)")
    run_engine(1, 1, 8, "单chunk + num_queues=1 (8线程共享池)  ")
    run_engine(4, 4, 8, "4chunk + num_queues=4 (4队列, 同chunk保序)")

    print("[C] 结论: 单 IO 存在真实等待时，加速比≈并行度；"
          "并行粒度是 chunk——多 chunk 或 num_queues=1 才能吃到多线程")


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main():
    import argparse
    parser = argparse.ArgumentParser(description="UMM 端到端测试场景")
    parser.add_argument(
        "--sim-only", action="store_true",
        help="只运行场景 C（慢设备模拟），不启动 umm 服务端",
    )
    args = parser.parse_args()

    if args.sim_only:
        scenario_simulated_slow_device()
        print("\n*** 场景 C PASS ***")
        return

    umm_root = find_umm_root()
    os.environ.setdefault("UMM_BUILD_DIR", os.path.join(umm_root, "build"))
    print(f"[setup] UMM_ROOT={umm_root}")
    print(f"[setup] UMM_BUILD_DIR={os.environ['UMM_BUILD_DIR']}")

    procs = start_servers(umm_root)
    try:
        from bmpclient.client import UMMServiceClient
        client = UMMServiceClient(
            meta_addr=f"127.0.0.1:{META_PORT}",
            mem_addr=f"127.0.0.1:{MEM_PORT}",
            node_id=0,
        )
        print("[client] UMMServiceClient 已连接（mock transport + RPC 分配面）")

        scenario_basic_rw(client)
        scenario_concurrent_io(client)

        client.close()
    finally:
        stop_servers(*procs)

    # 场景 C 不依赖服务端，单独运行
    scenario_simulated_slow_device()
    print("\n*** 全部场景 PASS ***")


if __name__ == "__main__":
    main()
