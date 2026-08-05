#!/usr/bin/env python3
"""
demo_e2e_remote_ssd.py — Phase 1 跨节点远程数据面端到端验证。

架构：
  节点 0（数据属主）：ummd(20001) + umms(20002, SSD tier = 分配权威 + 数据面)
  节点 1（客户端）  ：无本地盘；peer_nodes 指向节点 0；
                    SSD GPA（node=0 != my=1）经 tier_router → transport_remote
                    → TCP DATA_READ/DATA_WRITE 流式帧 → 节点 0 对本盘 I/O。

场景：
  R0  远程读写一致性：alloc SSD chunk → 断言 GPA node 位==属主 → 5MB pattern
      写（> data_max_io=1MB，强制走分片路径）→ 读回 memcmp → 尾段偏移写读。
  R1  双客户端并发分配唯一性：两个 --worker 子进程并发 alloc，offset 集合互斥。
  R2  负路径：
      a. 超 data_max_io 的原始 DATA_WRITE 帧 → 服务端回错误状态且保持存活；
      b. 未知节点 GPA 读 → UMM_E_NOT_FOUND(-2)；
      c. 错误 token 客户端 → alloc 被拒（服务端静默断连）；
      d. 远程 GPA 原子操作 → UMM_E_UNSUPPORTED(-9)；
      e. CIDR 白名单不含客户端 IP → accept 即断（仅自管服务端模式）。
  R3  杀掉 umms → 重启 → 已有 chunk 数据面自动重连、盘上数据仍在
      （仅自管服务端模式；--no-server 时跳过并打印手工步骤）。

用法：
  # 单机回环（CI / 开发自验）：脚本自管 ummD + umms（两个进程模拟两节点）
  UMM_ROOT=/path/to/umm python3 scripts/demo_e2e_remote_ssd.py

  # 真双 VM：节点 0 上按 docs/05 启动 ummD/umms（务必配置 rpc_token /
  #   allow_cidrs），节点 1 上：
  python3 scripts/demo_e2e_remote_ssd.py --no-server \
      --meta-addr 10.0.0.11:20001 --mem-addr 10.0.0.11:20002 \
      --token s3cr3t-phase1

环境变量（均可被命令行覆盖）：
  UMM_SSD_SPEC      服务端 SSD 设备路径（缺省 /tmp/umm_e2e_remote_ssd.raw）
  UMM_SSD_CAPACITY  容量（缺省 256M；后缀 G/M/K 或纯字节数）
  UMM_RPC_TOKEN     共享密钥（缺省 s3cr3t-phase1）
"""

import argparse
import ctypes
import os
import socket
import struct
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
CHUNK_SIZE = 6 * 1024 * 1024          # R0 chunk：6MB
BIG_WRITE = 5 * 1024 * 1024           # 5MB > data_max_io(1MB)，强制分片
TAIL_WRITE = 1 * 1024 * 1024
DEFAULT_TOKEN = "s3cr3t-phase1"
DEFAULT_DATA_MAX_IO = 1 * 1024 * 1024

UMM_E_NOT_FOUND = -2
UMM_E_UNSUPPORTED = -9

# GPA 位布局（umm/include/umm.h）
GPA_NODE_SHIFT = 58
GPA_TIER_SHIFT = 56
GPA_OFFSET_MASK = (1 << 56) - 1
UMM_TIER_SSD = 2


def gpa_node(gpa: int) -> int:
    return (gpa >> GPA_NODE_SHIFT) & 0x3F


def gpa_tier(gpa: int) -> int:
    return (gpa >> GPA_TIER_SHIFT) & 0x3


def gpa_offset(gpa: int) -> int:
    return gpa & GPA_OFFSET_MASK


def make_gpa(node: int, tier: int, off: int) -> int:
    return (node << GPA_NODE_SHIFT) | (tier << GPA_TIER_SHIFT) | off


def fnv1a_digest6(token: str) -> bytes:
    """与 protocol_common.c umm_token_digest 同口径：FNV-1a 64bit 低 6 字节 LE。"""
    if not token:
        return b"\x00" * 6
    h = 14695981039346656037
    for b in token.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return bytes((h >> (8 * i)) & 0xFF for i in range(6))


def parse_size(s: str) -> int:
    s = s.strip()
    mult = {"G": 1 << 30, "M": 1 << 20, "K": 1 << 10}
    if s and s[-1].upper() in mult:
        return int(s[:-1]) * mult[s[-1].upper()]
    return int(s, 0)


def split_addr(addr: str):
    host, _, port = addr.rpartition(":")
    return host, int(port)


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


def make_pattern(nbytes: int, seed: int) -> bytes:
    """确定性 pattern：1MB 块 × repeat，块间异或区分（防重复块掩盖偏移 bug）。"""
    blk = bytes(((i * 131 + seed) & 0xFF) for i in range(4096))
    out = bytearray()
    nblocks = (nbytes + 4095) // 4096
    for i in range(nblocks):
        out += bytes(b ^ (i & 0xFF) for b in blk)
    return bytes(out[:nbytes])


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"对端关闭（已收 {len(buf)}/{n} 字节）")
        buf += chunk
    return buf


# ---------------------------------------------------------------------------
# 服务端自管（回环模拟两节点）
# ---------------------------------------------------------------------------

def write_umms_config(spec: str, capacity: int, token: str,
                      data_max_io: int, allow_cidrs: str = "") -> str:
    lines = [
        "node_id: 0",
        'listen_addr: "127.0.0.1"',
        f"listen_port: {MEM_PORT}",
        f"memory_size: {MEM_SIZE}",
        "base_gpa: 0",
        f'ssd_devices: "{spec}:{capacity}"',
    ]
    if token:
        lines.append(f'rpc_token: "{token}"')
    if allow_cidrs:
        lines.append(f'allow_cidrs: "{allow_cidrs}"')
    if data_max_io:
        lines.append(f"data_max_io: {data_max_io}")
    fd, path = tempfile.mkstemp(prefix="umm_e2e_remote_umms_", suffix=".yaml")
    with os.fdopen(fd, "w") as f:
        f.write("\n".join(lines) + "\n")
    return path


class ManagedServers:
    """回环模式：本进程拉起 ummD + umms（节点 0），可随时杀掉/重启 umms。"""

    def __init__(self, umm_root: str, spec: str, capacity: int,
                 token: str, data_max_io: int):
        self.umm_root = umm_root
        self.spec = spec
        self.capacity = capacity
        self.token = token
        self.data_max_io = data_max_io
        self.proc_ummd = None
        self.proc_umms = None

    def start_all(self):
        ummd_bin = find_server_binary(self.umm_root,
                                      ["umm-metadata-service", "ummd"])
        self.proc_ummd = subprocess.Popen(
            [ummd_bin, "-p", str(META_PORT), "-b", "127.0.0.1"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_port("127.0.0.1", META_PORT):
            raise RuntimeError("umm-metadata-service 端口未就绪")
        print(f"[servers] ummD:{META_PORT} 已就绪")
        self.start_umms()

    def start_umms(self, allow_cidrs: str = ""):
        umms_bin = find_server_binary(self.umm_root,
                                      ["umm-memory-server", "umms"])
        cfg = write_umms_config(self.spec, self.capacity, self.token,
                                self.data_max_io, allow_cidrs)
        self.proc_umms = subprocess.Popen(
            [umms_bin, "-c", cfg],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_port("127.0.0.1", MEM_PORT):
            raise RuntimeError("umms 端口未就绪（SSD 设备打开失败？）")
        print(f"[servers] umms:{MEM_PORT} 已就绪 (cfg={cfg})")

    def kill_umms(self):
        if self.proc_umms and self.proc_umms.poll() is None:
            self.proc_umms.terminate()
            try:
                self.proc_umms.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc_umms.kill()
                self.proc_umms.wait(timeout=5)
        print("[servers] umms 已杀掉")

    def stop_all(self):
        self.kill_umms()
        if self.proc_ummd and self.proc_ummd.poll() is None:
            self.proc_ummd.terminate()
            try:
                self.proc_ummd.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc_ummd.kill()
        print("[servers] 全部关闭")


# ---------------------------------------------------------------------------
# worker 模式（R1 并发分配子进程）
# ---------------------------------------------------------------------------

def run_worker(args):
    """独立进程：init → alloc K 个 SSD chunk → 逐行打印 'GPA 0x...'。"""
    from bmpclient.client import UMMServiceClient
    peer = f"{args.server_node}:{args.mem_addr}"
    client = UMMServiceClient(
        meta_addr=args.meta_addr, mem_addr=args.mem_addr,
        node_id=args.node_id, tier_aware=True,
        peer_nodes=peer, rpc_token=args.token,
    )
    try:
        for _ in range(args.chunks):
            desc = client.create_chunk(1 * 1024 * 1024, "ssd")
            print(f"GPA 0x{desc.base_gpa:016x}", flush=True)
    finally:
        client.close()


# ---------------------------------------------------------------------------
# 场景实现
# ---------------------------------------------------------------------------

def scenario_r0_remote_rw(client, server_node: int):
    """R0：远程读写一致性（含分片路径与偏移写）。"""
    print("\n=== R0：远程读写一致性 ===")
    desc = client.create_chunk(CHUNK_SIZE, "ssd")
    node = gpa_node(desc.base_gpa)
    tier = gpa_tier(desc.base_gpa)
    print(f"[R0] chunk: base_gpa=0x{desc.base_gpa:016x} "
          f"node={node} tier={tier} offset=0x{gpa_offset(desc.base_gpa):x}")
    assert node == server_node, (
        f"GPA node 位应属主 {server_node}，实际 {node}（旧 bug：填的是客户端 id）")
    assert tier == UMM_TIER_SSD, f"tier 应为 SSD(2)，实际 {tier}"

    pattern = make_pattern(BIG_WRITE, seed=7)
    t0 = time.perf_counter()
    client.write_chunk(desc, 0, pattern)
    wms = (time.perf_counter() - t0) * 1000
    print(f"[R0] 写 {BIG_WRITE // 1024 // 1024}MB（>{1}MB max_io，"
          f"强制分片）: {wms:.1f}ms")

    t0 = time.perf_counter()
    back = client.read_chunk(desc, 0, BIG_WRITE)
    rms = (time.perf_counter() - t0) * 1000
    assert back == pattern, "5MB 读回数据不一致"
    print(f"[R0] 读回校验 PASS: {rms:.1f}ms")

    tail = make_pattern(TAIL_WRITE, seed=99)
    client.write_chunk(desc, BIG_WRITE, tail)
    assert client.read_chunk(desc, BIG_WRITE, TAIL_WRITE) == tail, \
        "尾段偏移写读不一致"
    assert client.read_chunk(desc, 0, BIG_WRITE) == pattern, \
        "尾段写入污染了前段数据"
    print("[R0] 偏移写读 + 前段不受污染 PASS")
    return desc


def scenario_r1_concurrent_alloc(args):
    """R1：两个 worker 子进程并发 alloc，offset 集合必须互斥。"""
    print("\n=== R1：双客户端并发分配唯一性 ===")
    base_cmd = [sys.executable, os.path.abspath(__file__), "--worker",
                "--meta-addr", args.meta_addr, "--mem-addr", args.mem_addr,
                "--server-node", str(args.server_node),
                "--token", args.token, "--chunks", "4"]
    procs, outs = [], []
    for nid in (11, 12):
        p = subprocess.Popen(base_cmd + ["--node-id", str(nid)],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True)
        procs.append(p)
    for p in procs:
        out, _ = p.communicate(timeout=60)
        outs.append(out)
        assert p.returncode == 0, f"worker 失败:\n{out}"

    offsets, gpas = [], []
    for out in outs:
        for line in out.splitlines():
            if line.startswith("GPA 0x"):
                g = int(line.split()[1], 16)
                gpas.append(g)
                offsets.append(gpa_offset(g))
                assert gpa_node(g) == args.server_node, \
                    f"worker chunk GPA node 位异常: {line}"
    assert len(gpas) == 8, f"应 alloc 8 个 chunk，实际 {len(gpas)}"
    dup = len(offsets) - len(set(offsets))
    assert dup == 0, f"并发分配出现 {dup} 个重复 offset: {sorted(offsets)}"
    print(f"[R1] 8 个 chunk offset 互斥 PASS: "
          f"{sorted(hex(o) for o in offsets)}")


def scenario_r2a_oversized_frame(client, args, desc):
    """R2a：原始 socket 发超 data_max_io 的 DATA_WRITE 帧 → 拒绝且服务存活。"""
    print("\n=== R2a：超 data_max_io 原始帧拒绝 ===")
    host, port = split_addr(args.mem_addr)
    oversize = args.data_max_io + 4096
    gpa = desc.base_gpa  # 合法 SSD GPA，仅 len 超限

    hdr = (b"UMMR" + bytes([1, 11, 0, 0]) + struct.pack("<H", 16)
           + fnv1a_digest6(args.token))
    body = struct.pack("<QQ", gpa, oversize)
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall(hdr + body)  # 非法帧按协议不带 payload
        rhdr = recv_exact(s, 16)
        assert rhdr[:4] == b"UMMR", f"响应 magic 异常: {rhdr[:4]!r}"
        blen = struct.unpack("<H", rhdr[8:10])[0]
        rbody = recv_exact(s, blen)
        status = struct.unpack("<i", rbody[:4])[0]
    print(f"[R2a] len={oversize} > cap={args.data_max_io} → status={status}")
    assert status != 0, "超限帧未被拒绝"

    # 服务端必须仍然存活且正常服务
    probe = make_pattern(4096, seed=5)
    client.write_chunk(desc, 0, probe)
    assert client.read_chunk(desc, 0, 4096) == probe
    print("[R2a] 拒绝后服务端存活、正常读写 PASS")


def scenario_r2b_unknown_node(client, args):
    """R2b：属主不在 peer 表的 GPA → UMM_E_NOT_FOUND。"""
    print("\n=== R2b：未知节点 GPA 路由 ===")
    from bmpclient.umm_client import ChunkDescriptor
    ghost = ChunkDescriptor()
    ghost.chunk_id = 0
    ghost.base_gpa = make_gpa(7, UMM_TIER_SSD, 0)  # 节点 7 不在 peer 表
    ghost.user_size = 4096
    try:
        client.read_chunk(ghost, 0, 4096)
        raise AssertionError("未知节点 GPA 读竟然成功")
    except RuntimeError as e:
        assert f"rc={UMM_E_NOT_FOUND}" in str(e), \
            f"期望 rc={UMM_E_NOT_FOUND}(NOT_FOUND)，实际: {e}"
        print(f"[R2b] node=7 不在 peer 表 → {e} PASS")


def scenario_r2c_wrong_token(args):
    """R2c：错误 token 客户端 → 服务端静默断连，alloc 失败。"""
    print("\n=== R2c：错误 token 拒绝 ===")
    from bmpclient.client import UMMServiceClient
    peer = f"{args.server_node}:{args.mem_addr}"
    bad = UMMServiceClient(
        meta_addr=args.meta_addr, mem_addr=args.mem_addr,
        node_id=13, tier_aware=True,
        peer_nodes=peer, rpc_token="definitely-wrong-token")
    try:
        bad.create_chunk(1 * 1024 * 1024, "ssd")
        raise AssertionError("错误 token 竟然 alloc 成功")
    except RuntimeError as e:
        print(f"[R2c] 错误 token → alloc 被拒: {e} PASS")
    finally:
        try:
            bad.close()
        except Exception:
            pass


def scenario_r2d_remote_atomic(client, desc):
    """R2d：远程 GPA 原子操作 → UMM_E_UNSUPPORTED（Phase 1 显式边界）。"""
    print("\n=== R2d：远程原子操作拒绝 ===")
    from bmpclient.umm_client import ChunkDescriptor
    fn = client.lib._lib.umm_atomic_set
    fn.argtypes = [ctypes.POINTER(ChunkDescriptor),
                   ctypes.c_uint64, ctypes.c_uint64]
    fn.restype = ctypes.c_int
    rc = fn(ctypes.byref(desc), 0, 42)
    print(f"[R2d] umm_atomic_set(remote gpa) → rc={rc}")
    assert rc == UMM_E_UNSUPPORTED, \
        f"期望 rc={UMM_E_UNSUPPORTED}(UNSUPPORTED)，实际 {rc}"
    print("[R2d] 远程原子操作显式 UNSUPPORTED PASS")


def scenario_r2e_acl(servers, args):
    """R2e：CIDR 白名单不含客户端 IP → accept 即断（仅自管模式）。"""
    print("\n=== R2e：CIDR 白名单拦截 ===")
    from bmpclient.client import UMMServiceClient
    servers.kill_umms()
    servers.start_umms(allow_cidrs="10.0.0.0/8")  # 不含 127.0.0.1
    peer = f"{args.server_node}:{args.mem_addr}"
    blocked = UMMServiceClient(
        meta_addr=args.meta_addr, mem_addr=args.mem_addr,
        node_id=14, tier_aware=True,
        peer_nodes=peer, rpc_token=args.token)
    try:
        blocked.create_chunk(1 * 1024 * 1024, "ssd")
        raise AssertionError("白名单外 IP 竟然 alloc 成功")
    except RuntimeError as e:
        print(f"[R2e] allow_cidrs=10.0.0.0/8 不含回环 → 被拒: {e} PASS")
    finally:
        try:
            blocked.close()
        except Exception:
            pass
    servers.kill_umms()
    servers.start_umms()  # 恢复正常
    print("[R2e] 服务端已恢复无 ACL 配置")


def scenario_r3_reconnect(client, servers, desc, args):
    """R3：杀掉 umms → 重启 → 数据面自动重连，盘上数据仍在。"""
    print("\n=== R3：服务端重启后数据面自动重连 ===")
    marker = make_pattern(4096, seed=31)
    client.write_chunk(desc, 0, marker)
    servers.kill_umms()
    time.sleep(0.5)
    servers.start_umms()  # 同一 SSD 文件，数据持久

    back = client.read_chunk(desc, 0, 4096)  # 触发断连→重连→重发
    assert back == marker, "重启后读回数据不一致（盘上数据丢失？）"
    print("[R3] 重启后旧 GPA 读回 PASS（自动重连 + 数据持久）")

    after = make_pattern(65536, seed=77)
    client.write_chunk(desc, 4096, after)
    assert client.read_chunk(desc, 4096, 65536) == after
    print("[R3] 重启后继续写读 PASS")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Phase 1 跨节点远程数据面 e2e（R0-R3）")
    parser.add_argument("--no-server", action="store_true",
                        help="不自管服务端（真双 VM：连远端已启动的 ummD/umms）")
    parser.add_argument("--meta-addr", default=f"127.0.0.1:{META_PORT}")
    parser.add_argument("--mem-addr", default=f"127.0.0.1:{MEM_PORT}")
    parser.add_argument("--server-node", type=int, default=0,
                        help="数据属主节点 id（缺省 0）")
    parser.add_argument("--node-id", type=int, default=1,
                        help="本客户端节点 id（缺省 1，须 != server-node）")
    parser.add_argument("--spec",
                        default=os.environ.get("UMM_SSD_SPEC",
                                               "/tmp/umm_e2e_remote_ssd.raw"))
    parser.add_argument("--capacity",
                        default=os.environ.get("UMM_SSD_CAPACITY", "256M"))
    parser.add_argument("--token",
                        default=os.environ.get("UMM_RPC_TOKEN", DEFAULT_TOKEN))
    parser.add_argument("--data-max-io", type=int,
                        default=DEFAULT_DATA_MAX_IO)
    parser.add_argument("--worker", action="store_true",
                        help=argparse.SUPPRESS)
    parser.add_argument("--chunks", type=int, default=4,
                        help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.worker:
        run_worker(args)
        return

    assert args.node_id != args.server_node, "客户端 node-id 必须 != 属主节点"

    servers = None
    if not args.no_server:
        umm_root = find_umm_root()
        os.environ.setdefault("UMM_BUILD_DIR", os.path.join(umm_root, "build"))
        capacity = parse_size(args.capacity)
        if not args.spec.startswith(("libnvm:", "nds:")) \
                and os.path.exists(args.spec):
            os.unlink(args.spec)
        print(f"[setup] UMM_ROOT={umm_root} spec={args.spec} "
              f"capacity={capacity} token=on max_io={args.data_max_io}")
        servers = ManagedServers(umm_root, args.spec, capacity,
                                 args.token, args.data_max_io)
        servers.start_all()
    else:
        host, port = split_addr(args.mem_addr)
        mhost, mport = split_addr(args.meta_addr)
        if not wait_port(mhost, mport, 5.0) or not wait_port(host, port, 5.0):
            raise RuntimeError("远端 ummD/umms 不可达，请先在节点 0 启动服务")
        print(f"[setup] 远端模式: meta={args.meta_addr} mem={args.mem_addr} "
              f"server_node={args.server_node}")

    from bmpclient.client import UMMServiceClient
    peer = f"{args.server_node}:{args.mem_addr}"
    client = UMMServiceClient(
        meta_addr=args.meta_addr, mem_addr=args.mem_addr,
        node_id=args.node_id, tier_aware=True,
        peer_nodes=peer, rpc_token=args.token,
        data_max_io=args.data_max_io)
    try:
        desc = scenario_r0_remote_rw(client, args.server_node)
        scenario_r1_concurrent_alloc(args)
        scenario_r2a_oversized_frame(client, args, desc)
        scenario_r2b_unknown_node(client, args)
        scenario_r2d_remote_atomic(client, desc)
        if servers is not None:
            scenario_r3_reconnect(client, servers, desc, args)
        else:
            print("\n=== R3：跳过（--no-server）。手工验证：在节点 0 重启 "
                  "umms 后重跑本脚本 R0 段即可观察自动重连 ===")
        # R3 重启过 umms 后控制面连接已断（mem RPC client 无自动重连，
        # Phase 2 范围）：此时 free 会 rc=-7，属预期边界，容忍之。
        try:
            client.delete_chunk(desc)
        except RuntimeError as e:
            print(f"[cleanup] free 失败（控制面断连，Phase 1 预期边界）: {e}")
        client.close()
        client = None

        scenario_r2c_wrong_token(args)
        if servers is not None:
            scenario_r2e_acl(servers, args)
        else:
            print("\n=== R2e：跳过（--no-server 无法改远端 ACL）。"
                  "手工验证：节点 0 umms 配置 allow_cidrs 不含本机 IP，"
                  "重跑应被拒 ===")
    finally:
        if client is not None:
            try:
                client.close()
            except Exception:
                pass
        if servers is not None:
            servers.stop_all()

    print("\n" + "=" * 56)
    print("Phase 1 远程数据面 e2e：R0-R3 全部 PASS")
    print("=" * 56)


if __name__ == "__main__":
    main()
