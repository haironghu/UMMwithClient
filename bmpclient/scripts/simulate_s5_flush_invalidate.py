#!/usr/bin/env python3
"""
simulate_s5_flush_invalidate.py — S5（A 写→fence 落盘→B invalidate→读回）单机仿真。

复刻双 VM S5 场景的编排时序（writer/preread/verify 三角色 + 标记文件带外
同步），用于代码路径回归：fence 落盘修正、umm_invalidate API、demo 的
S5 三角色逻辑、跨进程 GPA 带外传递。

与双 VM 实验的唯一语义差异（务必知悉）：
  单机两进程 mmap 同一文件（MAP_SHARED）共享同一份页缓存——writer 写入
  的页对 reader 立即可见，"刷盘前读到陈旧数据"的负路径在单机上不会复现
  （verify 会打印"提示"而非"负路径证据"）。陈旧页缓存陷阱需要两个独立
  内核的页缓存（两台 VM），负路径证据以 lab_shared_ssd/06 的双 VM 运行为准。
  本脚本断言的是：正路径（invalidate → 读回与写入端逐字节一致）必须成立。

断言：
  B1 writer/preread/verify 三进程 exit 0（writer 在 verify 完成后才释放退出）
  B2 verify 日志含 "刷盘后读回 digest 与写入端 pattern 完全一致 PASS"
  B3 所有客户端均未挂载 remote transport（共享盘门槛）
  B4（信息性）负路径是否复现：单机上预期不复现，打印说明

用法：
  UMM_ROOT=/path/to/umm python3 scripts/simulate_s5_flush_invalidate.py
  # 可选：--dev-size 128M --s5-size 16M --meta-port 31101 --mem-port 31102
"""

import argparse
import json
import os
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PKG_DIR = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.dirname(PKG_DIR))

from simulate_shared_pool import (  # noqa: E402
    find_umm_root, start_servers, stop_servers, wait_port, parse_size,
)


def wait_file(path: str, timeout_s: float, proc: subprocess.Popen, what: str):
    t0 = time.time()
    while not os.path.exists(path):
        if proc.poll() is not None:
            raise RuntimeError(f"{what} 进程已退出（rc={proc.returncode}），"
                               f"标记 {path} 未出现")
        if time.time() - t0 > timeout_s:
            raise RuntimeError(f"等待 {what} 超时（{timeout_s}s）：{path}")
        time.sleep(0.2)


def run_client(args, node_id, tag, mode, extra, workdir):
    cmd = [
        sys.executable, os.path.join(SCRIPT_DIR, "demo_shared_pool.py"),
        "--meta-addr", f"127.0.0.1:{args.meta_port}",
        "--mem-addr", f"127.0.0.1:{args.mem_port}",
        "--node-id", str(node_id),
        "--tag", tag, "--token", args.token,
        "--devices", f"{workdir}/ssd0.raw:{args.dev_size},"
                     f"{workdir}/ssd1.raw:{args.dev_size}",
        "--s5-mode", mode,
        "--s5-size", args.s5_size,
        "--s5-tag", tag,
        "--s5-dir", workdir,
        "--s5-timeout", str(args.timeout),
    ] + extra
    env = dict(os.environ)
    return subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, env=env)


def main():
    ap = argparse.ArgumentParser(description="S5 落盘/刷盘语义：单机仿真编排")
    ap.add_argument("--dev-size", default="128M")
    ap.add_argument("--s5-size", default="16M")
    ap.add_argument("--meta-port", type=int, default=31101)
    ap.add_argument("--mem-port", type=int, default=31102)
    ap.add_argument("--token", default="s5-token")
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    umm_root = find_umm_root()
    os.environ.setdefault("UMM_BUILD_DIR", os.path.join(umm_root, "build"))
    dev_size = parse_size(args.dev_size)

    import tempfile
    workdir = tempfile.mkdtemp(prefix="umm_sim_s5_")
    dev0 = os.path.join(workdir, "ssd0.raw")
    dev1 = os.path.join(workdir, "ssd1.raw")
    for d in (dev0, dev1):
        with open(d, "wb") as f:
            f.truncate(dev_size)
    print(f"[setup] workdir={workdir} dev=2x{dev_size // (1 << 20)}MB "
          f"s5_chunk={args.s5_size}")

    procs = start_servers(umm_root, args.meta_port, args.mem_port,
                          dev0, dev1, dev_size, args.token)
    outputs = {}
    try:
        # 1) writer（node 0）：alloc → 等预读 → 写 → fence 落盘 → 保活
        writer = run_client(args, 0, "simS5", "writer", [], workdir)
        wait_file(os.path.join(workdir, "s5_alloc_done"),
                  args.timeout, writer, "writer 分配")
        rec = json.load(open(os.path.join(workdir, "s5_alloc.json")))
        print(f"[s5] writer 已分配 chunk_id={rec['chunk_id']} gpa={rec['gpa']}")

        # 2) preread（node 1）：填充本机页缓存旧视图 → 放行 writer
        pre = run_client(args, 1, "simS5", "preread",
                         ["--s5-gpa", rec["gpa"],
                          "--s5-chunk-id", str(rec["chunk_id"])], workdir)
        outputs["preread"] = pre.communicate(timeout=args.timeout)[0]
        assert pre.returncode == 0, \
            f"B1 失败: preread exit={pre.returncode}\n{outputs['preread']}"
        open(os.path.join(workdir, "s5_preread_done"), "w").close()

        # 3) 等 writer 写完并 fence 落盘
        wait_file(os.path.join(workdir, "s5_written"),
                  args.timeout, writer, "writer 写入")
        print("[s5] writer 写入完成并已 fence 落盘（进程保活中）")

        # 4) verify（node 1）：刷盘前读 → invalidate → 刷盘后读
        ver = run_client(args, 1, "simS5", "verify",
                         ["--s5-gpa", rec["gpa"],
                          "--s5-chunk-id", str(rec["chunk_id"])], workdir)
        outputs["verify"] = ver.communicate(timeout=args.timeout)[0]
        assert ver.returncode == 0, \
            f"B1 失败: verify exit={ver.returncode}\n{outputs['verify']}"
        assert "刷盘后读回 digest 与写入端 pattern 完全一致 PASS" \
            in outputs["verify"], \
            f"B2 失败: verify 缺少 PASS 证据\n{outputs['verify']}"
        print("[B2] 正路径 PASS：invalidate 刷盘后读回与写入端逐字节一致")

        if "负路径证据：刷盘前读到陈旧数据" in outputs["verify"]:
            print("[B4] 负路径复现：刷盘前读到陈旧数据（单机上罕见，"
                  "说明两进程未共享相关缓存页）")
        else:
            print("[B4] 预期内：单机共享页缓存，负路径（陈旧读）不复现；"
                  "负路径证据以双 VM 实验（lab_shared_ssd/06）为准")

        # 5) 放行 writer 释放 chunk，收退出码
        open(os.path.join(workdir, "s5_verify_done"), "w").close()
        outputs["writer"] = writer.communicate(timeout=args.timeout)[0]
        assert writer.returncode == 0, \
            f"B1 失败: writer exit={writer.returncode}\n{outputs['writer']}"
        assert "chunk 已释放" in outputs["writer"], outputs["writer"]
        print("[B1] 三角色全部 exit 0，writer 在 verify 完成后才释放 chunk")

        # B3：共享盘门槛
        for name, out in outputs.items():
            assert "remote transport attached" not in out, \
                f"B3 失败: {name} 挂载了 remote transport\n{out}"
        print("[B3] 三角色均未挂载 remote transport PASS")
    finally:
        stop_servers(*procs)

    print("\n========================================================")
    print("S5 落盘/刷盘语义：单机仿真 B1-B4 全部 PASS")
    print("========================================================")


if __name__ == "__main__":
    main()
