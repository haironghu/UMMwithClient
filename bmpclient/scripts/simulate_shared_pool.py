#!/usr/bin/env python3
"""
simulate_shared_pool.py — 共享盘池实验：单机仿真编排（双 VM 拓扑的宿主机等价物）。

在单机上复刻双 VM 实验拓扑：
  - 两个 raw 稀疏文件 = QEMU 共享 SSD 的等价物（同一后端、双侧打开）
  - ummD + umms（node 0）= VM1 的 UMM 服务
  - 两个并发客户端进程（node 0 / node 1）= 两个 VM 的 UMM 客户端

编排断言（比单节点脚本更强的集群级验证）：
  A1 两个客户端进程 exit 0（各自的 S0-S3 全 PASS）
  A2 集群级 offset 唯一性：两侧 chunk 的 [offset, offset+size) 区间两两不相交
  A3 共享盘门槛：node 1 客户端日志中不得出现 "remote transport attached"
     （未配 peer_nodes → remote 不挂载 → I/O 必走本机盘，不过网络）
  A4 盘上数据校验：按 offset 把 chunk 映射回 raw 文件（含跨设备分段），
     与确定性 pattern 逐字节比对——证明数据真的落到了正确的"盘"上

用法：
  UMM_ROOT=/path/to/umm python3 scripts/simulate_shared_pool.py
  # 可选：--dev-size 64M --chunks 4 --chunk-size 2M --meta-port 21001 --mem-port 21002
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PKG_DIR = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.dirname(PKG_DIR))

from demo_shared_pool import pattern_piece, parse_size  # noqa: E402


def find_umm_root() -> str:
    env = os.environ.get("UMM_ROOT")
    cands = [env] if env else []
    cands += [os.path.join(PKG_DIR, "..", "umm"),
              os.path.join(PKG_DIR, "..", "..", "umm")]
    for p in cands:
        if p and os.path.isdir(os.path.join(p, "bin")):
            return os.path.abspath(p)
    raise FileNotFoundError(f"找不到 UMM 根目录: {cands}")


def wait_port(port: int, timeout: float = 8.0) -> bool:
    import socket
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def _runnable(binary: str) -> str:
    """源码树可能落在 noexec 挂载上（如 /mnt fuse）：拷到 /tmp 再执行。"""
    if os.access(binary, os.X_OK):
        return binary
    dst = os.path.join(tempfile.gettempdir(),
                       f"umm_sim_{os.path.basename(binary)}")
    shutil.copy2(binary, dst)
    os.chmod(dst, 0o755)
    return dst


def start_servers(umm_root, meta_port, mem_port, dev0, dev1, dev_size, token):
    ummd = _runnable(os.path.join(umm_root, "bin", "ummd"))
    umms = _runnable(os.path.join(umm_root, "bin", "umms"))
    cfg = tempfile.NamedTemporaryFile(
        mode="w", prefix="umm_sim_umms_", suffix=".yaml", delete=False)
    cfg.write(
        "node_id: 0\n"
        'listen_addr: "127.0.0.1"\n'
        f"listen_port: {mem_port}\n"
        f"memory_size: {64 * 1024 * 1024}\n"
        "base_gpa: 0\n"
        f'ssd_devices: "{dev0}:{dev_size},{dev1}:{dev_size}"\n'
        f'rpc_token: "{token}"\n'
    )
    cfg.close()
    p1 = subprocess.Popen([ummd, "-p", str(meta_port), "-b", "127.0.0.1"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p2 = subprocess.Popen([umms, "-c", cfg.name],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not wait_port(meta_port) or not wait_port(mem_port):
        raise RuntimeError("服务端端口未就绪")
    print(f"[servers] ummD:{meta_port} umms:{mem_port} 已就绪 "
          f"(pool=2x{dev_size // (1 << 20)}MB)")
    return p1, p2


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


def verify_on_disk(results, dev_paths, dev_size):
    """A4：chunk → raw 文件映射（含跨设备分段），逐字节比对 pattern。"""
    checked = 0
    for res in results:
        tag = res["tag"]
        for ch in res["chunks"]:
            off, size = ch["offset"], ch["size"]
            pos = 0
            while pos < size:
                dev_idx = 0 if (off + pos) < dev_size else 1
                dev_base = dev_idx * dev_size
                seg = min(size - pos, dev_base + dev_size - (off + pos))
                expect = b""
                # 逐 1MB piece 重组（pattern 以 1MB piece 为种子单位）
                while len(expect) < seg:
                    rel = pos + len(expect)
                    pidx = rel // (1 << 20)
                    poff = rel % (1 << 20)
                    take = min(seg - len(expect), (1 << 20) - poff)
                    expect += pattern_piece(tag, ch["chunk_id"], pidx)[poff:poff + take]
                with open(dev_paths[dev_idx], "rb") as f:
                    f.seek(off + pos - dev_base)
                    actual = f.read(seg)
                assert actual == expect, (
                    f"A4 失败: {tag} chunk{ch['chunk_id']} "
                    f"dev{dev_idx}@0x{off + pos - dev_base:x} "
                    f"len={seg}（前 16B expect={expect[:16].hex()} "
                    f"actual={actual[:16].hex()}）")
                pos += seg
            checked += 1
    print(f"[A4] {checked} 个 chunk 盘上内容逐字节校验 PASS（含跨设备分段）")


def main():
    ap = argparse.ArgumentParser(description="共享盘池实验：单机仿真编排")
    # 默认参数保证 offset=15M 的 chunk 跨 16M 设备边界（straddle 覆盖）
    ap.add_argument("--dev-size", default="16M")
    ap.add_argument("--chunks", type=int, default=3)
    ap.add_argument("--chunk-size", default="3M")
    ap.add_argument("--meta-port", type=int, default=21001)
    ap.add_argument("--mem-port", type=int, default=21002)
    ap.add_argument("--token", default="sim-token")
    args = ap.parse_args()

    dev_size = parse_size(args.dev_size)
    workdir = tempfile.mkdtemp(prefix="umm_sim_shared_")
    dev0 = os.path.join(workdir, "ssd0.raw")
    dev1 = os.path.join(workdir, "ssd1.raw")
    print(f"[setup] workdir={workdir} dev=2x{dev_size // (1 << 20)}MB")

    umm_root = find_umm_root()
    os.environ.setdefault("UMM_BUILD_DIR", os.path.join(umm_root, "build"))

    procs = start_servers(umm_root, args.meta_port, args.mem_port,
                          dev0, dev1, dev_size, args.token)
    try:
        # 并发启动两个"VM"客户端
        clients = []
        for node_id, tag in ((0, "simA"), (1, "simB")):
            out_json = os.path.join(workdir, f"result_{tag}.json")
            cmd = [
                sys.executable,
                os.path.join(SCRIPT_DIR, "demo_shared_pool.py"),
                "--meta-addr", f"127.0.0.1:{args.meta_port}",
                "--mem-addr", f"127.0.0.1:{args.mem_port}",
                "--node-id", str(node_id),
                "--tag", tag,
                "--token", args.token,
                "--devices", f"{dev0}:{dev_size},{dev1}:{dev_size}",
                "--chunks", str(args.chunks),
                "--chunk-size", args.chunk_size,
                "--out", out_json,
                "--keep",
            ]
            env = dict(os.environ)
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, env=env)
            clients.append((tag, p, out_json))

        outputs = {}
        for tag, p, _ in clients:
            out, _ = p.communicate(timeout=300)
            outputs[tag] = out
            assert p.returncode == 0, f"A1 失败: {tag} 客户端 exit={p.returncode}\n{out}"
        print("[A1] 两个客户端进程 S0-S3 全部 exit 0 PASS")

        # A2：集群级 offset 不相交
        results = []
        intervals = []
        for tag, _, out_json in clients:
            with open(out_json) as f:
                res = json.load(f)
            results.append(res)
            for ch in res["chunks"]:
                intervals.append((tag, ch["offset"],
                                  ch["offset"] + ch["size"]))
        intervals.sort(key=lambda x: x[1])
        for i in range(1, len(intervals)):
            assert intervals[i][1] >= intervals[i - 1][2], (
                f"A2 失败: 分配区间相交 {intervals[i - 1]} vs {intervals[i]}")
        assert all(iv[2] <= 2 * dev_size for iv in intervals), "offset 超出池容量"
        print(f"[A2] {len(intervals)} 个 chunk 区间集群级两两不相交 PASS")

        # A3：node 1 日志不得出现 remote transport 挂载
        assert "remote transport attached" not in outputs["simB"], (
            "A3 失败: node 1 挂载了 remote transport（共享盘模型下不应出现）\n"
            + outputs["simB"])
        assert "no route to owner" not in outputs["simB"], (
            "A3 失败: node 1 尝试远端路由\n" + outputs["simB"])
        print("[A3] node 1 未挂载 remote transport（I/O 全部本机盘）PASS")

        # A4：盘上内容逐字节校验
        verify_on_disk(results, (dev0, dev1), dev_size)
    finally:
        stop_servers(*procs)

    print("\n" + "=" * 56)
    print("共享盘池实验：单机仿真 A1-A4 全部 PASS")
    print("=" * 56)


if __name__ == "__main__":
    main()
