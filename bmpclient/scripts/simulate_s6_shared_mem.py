#!/usr/bin/env python3
"""
simulate_s6_shared_mem.py — S6 共享内存窗口（硬件一致，无需 invalidate）单机仿真。

验证 Phase 2.5 的代码路径：内存层后备从私有 malloc 换成共享窗口文件后，
writer（node 0）写入 → verify（node 1）不执行任何 invalidate 直接读回，
digest 必须逐字节一致。覆盖两条路线：
  路线A：tier=1（CXL 分支真实设备路径，open+mmap 而非 mock 回退）
  路线B：tier=0（DRAM 分支设备后备，注册即 mmap，失败不回退）

与双 VM 实验的语义差异（务必知悉）：
  单机两进程 mmap 同一文件共享同一份页缓存，"一致"由内核页缓存天然保证；
  双 VM 下 virtio-pmem 的 DAX 映射直达宿主共享物理页，一致性由 x86 硬件
  cacheline 协议保证——两条路径机制不同但结论同构（都不需要 invalidate）。
  本仿真验证的是代码接线（设备路径贯穿 umms/客户端、无 malloc 回退、
  digest 一致、时序正确），硬件一致性声明以 lab_shared_ssd/06 的双 VM
  运行（virtio-pmem /dev/pmem0）为准。

断言（每条路线）：
  C1 writer/verify 两进程 exit 0
  C2 verify 日志含 "未执行任何 invalidate" PASS 行
  C3 全链路无 "falling back to malloc backing"（静默回退守卫）
     且无 "remote transport attached"（共享盘门槛）

用法：
  UMM_ROOT=/path/to/umm python3 scripts/simulate_s6_shared_mem.py
"""

import json
import os
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PKG_DIR = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.dirname(PKG_DIR))

from simulate_shared_pool import find_umm_root, stop_servers, wait_port  # noqa: E402
from simulate_s5_flush_invalidate import wait_file  # noqa: E402

SHM_SIZE = 128 * 1024 * 1024
DEV_SIZE = 64 * 1024 * 1024
S6_SIZE = "16M"
TIMEOUT = 120


def start_servers_with_shm(umm_root, meta_port, mem_port, dev0, dev1,
                           shm, mem_tier_kind, token):
    """start_servers 的共享窗口变体：umms.yaml 追加 memory_tier/memory_device。"""
    ummd = os.path.join(umm_root, "bin", "ummd")
    umms = os.path.join(umm_root, "bin", "umms")
    for b in (ummd, umms):
        if not (os.path.isfile(b) and os.access(b, os.X_OK)):
            raise FileNotFoundError(f"二进制不可执行: {b}（先 make）")
    cfg = tempfile.NamedTemporaryFile(
        mode="w", prefix="umm_sim_s6_umms_", suffix=".yaml", delete=False)
    cfg.write(
        "node_id: 0\n"
        'listen_addr: "127.0.0.1"\n'
        f"listen_port: {mem_port}\n"
        f"memory_size: {SHM_SIZE}\n"
        f'memory_tier: "{mem_tier_kind}"\n'
        f'memory_device: "{shm}"\n'
        "base_gpa: 0\n"
        f'ssd_devices: "{dev0}:{DEV_SIZE},{dev1}:{DEV_SIZE}"\n'
        f'rpc_token: "{token}"\n'
    )
    cfg.close()
    p1 = subprocess.Popen([ummd, "-p", str(meta_port), "-b", "127.0.0.1"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p2 = subprocess.Popen([umms, "-c", cfg.name],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not wait_port(meta_port) or not wait_port(mem_port):
        raise RuntimeError(f"服务端端口未就绪（mem_tier={mem_tier_kind}）")
    return p1, p2


def run_client(args, workdir, shm, node_id, mode, extra, meta_port, mem_port):
    cmd = [
        sys.executable, os.path.join(SCRIPT_DIR, "demo_shared_pool.py"),
        "--meta-addr", f"127.0.0.1:{meta_port}",
        "--mem-addr", f"127.0.0.1:{mem_port}",
        "--node-id", str(node_id),
        "--tag", "simS6", "--token", "s6-token",
        "--devices", f"{workdir}/ssd0.raw:{DEV_SIZE},"
                     f"{workdir}/ssd1.raw:{DEV_SIZE}",
        "--mem-tier", str(args["mem_tier"]),
        "--mem-tier-size", str(SHM_SIZE),
        "--mem-device", shm,
        "--s6-mode", mode,
        "--s6-size", S6_SIZE,
        "--s6-tag", "simS6",
        "--s6-dir", workdir,
        "--s6-timeout", str(TIMEOUT),
    ] + extra
    env = dict(os.environ)
    return subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, env=env)


def run_route(umm_root, workdir, shm, mem_tier_kind, mem_tier, ports):
    print(f"\n----- 路线{'A' if mem_tier == 1 else 'B'}："
          f"memory_tier={mem_tier_kind} tier={mem_tier} -----")
    dev0 = os.path.join(workdir, "ssd0.raw")
    dev1 = os.path.join(workdir, "ssd1.raw")
    for d, sz in ((dev0, DEV_SIZE), (dev1, DEV_SIZE), (shm, SHM_SIZE)):
        with open(d, "wb") as f:
            f.truncate(sz)

    procs = start_servers_with_shm(umm_root, ports[0], ports[1],
                                   dev0, dev1, shm, mem_tier_kind, "s6-token")
    args = {"mem_tier": mem_tier}
    outputs = {}
    try:
        for m in ("s6_alloc_done", "s6_written", "s6_verify_done",
                  "s6_alloc.json"):
            p = os.path.join(workdir, m)
            if os.path.exists(p):
                os.unlink(p)

        writer = run_client(args, workdir, shm, 0, "writer", [], *ports)
        wait_file(os.path.join(workdir, "s6_alloc_done"),
                  TIMEOUT, writer, "writer 分配")
        rec = json.load(open(os.path.join(workdir, "s6_alloc.json")))
        assert rec["tier"] == mem_tier, f"tier 不符: {rec}"
        print(f"[s6] writer 已分配 chunk_id={rec['chunk_id']} "
              f"gpa={rec['gpa']} tier={rec['tier']}")

        wait_file(os.path.join(workdir, "s6_written"),
                  TIMEOUT, writer, "writer 写入")

        ver = run_client(args, workdir, shm, 1, "verify",
                         ["--s6-gpa", rec["gpa"],
                          "--s6-chunk-id", str(rec["chunk_id"])], *ports)
        outputs["verify"] = ver.communicate(timeout=TIMEOUT)[0]
        assert ver.returncode == 0, \
            f"C1 失败: verify exit={ver.returncode}\n{outputs['verify']}"
        assert "未执行任何 invalidate" in outputs["verify"] \
               and "PASS" in outputs["verify"], \
            f"C2 失败: verify 缺少 PASS 证据\n{outputs['verify']}"
        print("[C2] 正路径 PASS：verify 未执行 invalidate，读回逐字节一致")

        open(os.path.join(workdir, "s6_verify_done"), "w").close()
        outputs["writer"] = writer.communicate(timeout=TIMEOUT)[0]
        assert writer.returncode == 0, \
            f"C1 失败: writer exit={writer.returncode}\n{outputs['writer']}"
        print("[C1] writer/verify 均 exit 0")

        for name, out in outputs.items():
            assert "falling back to malloc backing" not in out, \
                f"C3 失败: {name} 回退了 malloc 私有后备（共享未生效）\n{out}"
            assert "remote transport attached" not in out, \
                f"C3 失败: {name} 挂载了 remote transport\n{out}"
        print("[C3] 守卫 PASS：无 malloc 回退、无 remote transport")
    finally:
        stop_servers(*procs)


def main():
    umm_root = find_umm_root()
    os.environ.setdefault("UMM_BUILD_DIR", os.path.join(umm_root, "build"))
    workdir = tempfile.mkdtemp(prefix="umm_sim_s6_")
    shm = os.path.join(workdir, "dram_shared.raw")
    print(f"[setup] workdir={workdir} shm=128MB s6_chunk={S6_SIZE}")

    # 路线A（tier=1，CXL 分支真实设备）与路线B（tier=0，DRAM 设备后备）
    run_route(umm_root, workdir, shm, "cxl", 1, (32101, 32102))
    run_route(umm_root, workdir, shm, "dram", 0, (32101, 32102))

    print("\n========================================================")
    print("S6 共享内存窗口：单机仿真（路线A+B）C1-C3 全部 PASS")
    print("========================================================")


if __name__ == "__main__":
    main()
