#!/usr/bin/env python3
"""
demo_shared_pool.py — 共享盘池实验：单节点客户端（每个 VM 各跑一份）。

实验模型（无跨节点读写/搬运）：
  两块 SSD 经 QEMU 同时挂给两个 VM（同一后端文件，share-rw）。
  umms（VM1）是两盘 pool 的分配权威；两个 VM 各自 RPC 申请、
  对分到的 offset 区间在本机盘上直接 I/O。数据不过网络。

本脚本在单个 VM 上执行：
  S0  拓扑可见性：get_device_list 应能看到 SSD 资源（ummD 全局视图）
  S1  申请 N 个 SSD chunk → 全量确定性 pattern 写入 → 读回逐字节校验
      （pattern 含节点 tag + chunk_id，集群内两节点内容必然不同——
        一旦分配重叠互相覆盖，校验必然失败）
  S2  写出结果 JSON（chunk_id / GPA / node / offset / size），
      供编排侧做集群级 offset 唯一性断言
  S3  负路径：超额申请 → NO_MEMORY；越界写 → 拒绝；重复释放 → 拒绝

用法（每个 VM 一份，node-id 不同、devices 顺序必须与 umms 一致）：
  python3 demo_shared_pool.py \
      --meta-addr 10.0.0.11:20001 --mem-addr 10.0.0.11:20002 \
      --node-id 0 --tag vm1 --token lab-token \
      --devices "/dev/nvme0n1:8G,/dev/nvme1n1:8G" \
      --chunks 2 --chunk-size 3G --out /tmp/result_vm1.json
（chunk 3G 时 offset=6G 的分配会跨越 8G 设备边界，覆盖跨设备分段路径；
 写读按 1MB piece 流式进行，内存占用与 chunk 大小无关。）

混合池（内存层+SSD，无 CXL 硬件）：加 --mem-tier/--mem-chunks 等参数，
追加 S1m（内存层分配+写读校验）与 S4（tier 容量隔离）：
  ... --mem-tier 1 --mem-chunks 1 --mem-chunk-size 128M --mem-tier-size 512M
路线A（零代码改动）：--mem-tier 1（mock CXL 槽位，malloc 后备=事实 DRAM）；
路线B（真 DRAM tier）：--mem-tier 0。注意内存层语义是"全局分配、私有
数据面"——umms 统一发 offset，但各 VM 写自己的 malloc buffer（与 SSD
层的物理共享不同；DRAM 本就不可能跨机共享）。

S5（落盘/刷盘语义：A 写 → fence 落盘 → B invalidate → 读回）：
三角色由编排脚本（06）分时启动，经 --s5-dir 下的标记文件带外同步：
  writer  ：vmA 分配 SSD chunk → 等对端预读 → 写 pattern → flush()
            （fence=CPU 屏障+全池 msync(MS_SYNC) 落盘）→ 进程保活等对端
            验证完才释放（fence 而非退出 munmap 才是被验证的落盘路径）
  preread ：vmB 预读目标区域（用旧内容填充本 VM 页缓存=制造陈旧视图）
  verify  ：vmB 先不刷盘读（若与写入 pattern 不符=页缓存陷阱负路径证据）
            → invalidate_chunk()（msync(MS_INVALIDATE) 丢弃本机缓存页）
            → 再读，digest 必须与写入端 pattern 完全一致（正路径断言）
B 侧未参与分配，chunk 描述符由 --s5-gpa/--s5-chunk-id/--s5-size 手工构造。

S6（共享内存窗口对照实验，Phase 2.5）：
内存层后备从私有 malloc 换成共享窗口（virtio-pmem /dev/pmem0，两侧
--mem-device 指向同一窗口）后，A 写 → B **直接读**（不 invalidate）
即应逐字节一致——DAX mmap 直达宿主共享物理页，x86 硬件 cacheline
一致性让两个 vCPU 等同 SMP 线程。与 S5（页缓存隔着，必须 fence+
invalidate）形成对照。writer 写完仅做 CPU 屏障（内存层 fence 语义），
进程保活等对端验完才释放。

注意：本脚本不设 peer_nodes —— 共享盘模型下 remote transport 不挂载
（opt-in 门槛），一切 I/O 落在本机盘。块设备需 UMM_ALLOW_BLOCK_DEVICE=1。
"""

import argparse
import hashlib
import json
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PKG_DIR = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.dirname(PKG_DIR))

GPA_NODE_SHIFT = 58
GPA_TIER_SHIFT = 56
GPA_OFFSET_MASK = (1 << 56) - 1
UMM_TIER_DRAM = 0
UMM_TIER_CXL = 1
UMM_TIER_SSD = 2
PIECE = 1 * 1024 * 1024


def gpa_node(g): return (g >> GPA_NODE_SHIFT) & 0x3F
def gpa_tier(g): return (g >> GPA_TIER_SHIFT) & 0x3
def gpa_offset(g): return g & GPA_OFFSET_MASK


def parse_size(s: str) -> int:
    s = s.strip()
    mult = {"G": 1 << 30, "M": 1 << 20, "K": 1 << 10}
    if s and s[-1].upper() in mult:
        return int(s[:-1]) * mult[s[-1].upper()]
    return int(s, 0)


def parse_devices(spec: str):
    """"path:size,path:size" -> [(path, bytes), ...]（保序）"""
    devs = []
    for entry in spec.split(","):
        entry = entry.strip()
        if not entry:
            continue
        path, _, size = entry.rpartition(":")
        assert path and size, f"设备条目格式错误: {entry!r}（期望 path:size）"
        devs.append((path, parse_size(size)))
    assert devs, "至少需要一个设备"
    return devs


def pattern_piece(tag: str, chunk_id: int, piece_idx: int,
                  size: int = PIECE) -> bytes:
    """确定性 pattern：sha256(tag|chunk_id|piece_idx) 循环填满。
    不同节点（tag 不同）内容必然不同 → 分配重叠必被校验发现。"""
    seed = hashlib.sha256(f"{tag}|{chunk_id}|{piece_idx}".encode()).digest()
    return (seed * (size // len(seed) + 1))[:size]


def chunk_pieces(tag: str, chunk_id: int, chunk_size: int):
    """逐 1MB piece 流式生成确定性 pattern（内存占用 O(1MB)，
    支持 GB 级 chunk——VM 实验需要大 chunk 跨越 8G 设备边界）。"""
    npieces = (chunk_size + PIECE - 1) // PIECE
    for p in range(npieces):
        yield p, pattern_piece(tag, chunk_id, p)[:min(PIECE, chunk_size - p * PIECE)]


def write_chunk_stream(client, desc, tag: str, chunk_size: int):
    for p, piece in chunk_pieces(tag, desc.chunk_id, chunk_size):
        client.write_chunk(desc, p * PIECE, piece)


def verify_chunk_stream(client, desc, tag: str, chunk_size: int, label: str):
    for p, piece in chunk_pieces(tag, desc.chunk_id, chunk_size):
        back = client.read_chunk(desc, p * PIECE, len(piece))
        assert back == piece, f"{label}: piece{p} 读回校验失败"


def scenario_s1_rw(client, tag: str, chunks: int, chunk_size: int):
    """S1：申请 → 全量写（流式）→ 读回校验 → 交叉复查（防后写覆盖先写）。"""
    descs = []
    print(f"[S1] 申请 {chunks} 个 SSD chunk x {chunk_size // 1024 // 1024}MB")
    for i in range(chunks):
        desc = client.create_chunk(chunk_size, "ssd")
        assert gpa_tier(desc.base_gpa) == UMM_TIER_SSD, \
            f"chunk{i} 不在 SSD tier: gpa=0x{desc.base_gpa:016x}"
        write_chunk_stream(client, desc, tag, chunk_size)
        descs.append(desc)
        print(f"[S1] chunk{i}: id={desc.chunk_id} "
              f"gpa=0x{desc.base_gpa:016x} node={gpa_node(desc.base_gpa)} "
              f"off=0x{gpa_offset(desc.base_gpa):x} 写入完成")

    for i, desc in enumerate(descs):
        verify_chunk_stream(client, desc, tag, chunk_size, f"chunk{i}")
    # 交叉复查：全部写完后重读第一个 chunk，排除后写覆盖先写
    verify_chunk_stream(client, descs[0], tag, chunk_size, "交叉复查chunk0")
    print(f"[S1] {chunks} 个 chunk 全量写读校验 + 交叉复查 PASS")
    return descs


def scenario_s3_negative(client, total_capacity: int, chunk_size: int):
    """S3：负路径三件套。"""
    print("[S3] 负路径：超额申请 / 越界写 / 重复释放")

    try:
        client.create_chunk(total_capacity + chunk_size, "ssd")
        raise AssertionError("超额申请竟然成功")
    except RuntimeError as e:
        assert "rc=-3" in str(e), f"期望 NO_MEMORY(-3)，实际: {e}"
        print(f"[S3] 超额申请被拒: {e}")

    # victim 用小 chunk：两 VM 并发跑 S3 时若按 chunk_size（可能数 GB）
    # 申请，池内空闲空间不足以同时满足两个 victim，会引入无关失败。
    victim_size = min(chunk_size, 16 * 1024 * 1024)
    victim = client.create_chunk(victim_size, "ssd")
    try:
        client.write_chunk(victim, victim_size - 1024, b"\xAA" * 4096)
        raise AssertionError("越界写竟然成功")
    except RuntimeError as e:
        print(f"[S3] 越界写被拒: {e}")

    client.delete_chunk(victim)
    try:
        client.delete_chunk(victim)
        raise AssertionError("重复释放竟然成功")
    except RuntimeError as e:
        print(f"[S3] 重复释放被拒: {e}")


def scenario_s1m_mem_rw(client, tag: str, mem_tier: int,
                        chunks: int, chunk_size: int):
    """S1m：内存层（DRAM/mock-CXL）分配 + 流式写读校验 + 交叉复查。"""
    descs = []
    print(f"[S1m] 申请 {chunks} 个内存层 chunk x "
          f"{chunk_size // 1024 // 1024}MB（tier={mem_tier}）")
    for i in range(chunks):
        desc = client.create_chunk_on_tier(chunk_size, mem_tier)
        assert gpa_tier(desc.base_gpa) == mem_tier, \
            f"mem chunk{i} tier 位错误: gpa=0x{desc.base_gpa:016x}"
        write_chunk_stream(client, desc, f"{tag}-m{i}", chunk_size)
        descs.append(desc)
        print(f"[S1m] chunk{i}: id={desc.chunk_id} "
              f"gpa=0x{desc.base_gpa:016x} tier={gpa_tier(desc.base_gpa)} "
              f"off=0x{gpa_offset(desc.base_gpa):x} 写入完成")

    for i, desc in enumerate(descs):
        verify_chunk_stream(client, desc, f"{tag}-m{i}", chunk_size,
                            f"mem chunk{i}")
    verify_chunk_stream(client, descs[0], f"{tag}-m0", chunk_size,
                        "交叉复查mem chunk0")
    print(f"[S1m] {chunks} 个内存层 chunk 全量写读校验 + 交叉复查 PASS")
    return descs


def scenario_s4_tier_isolation(client, mem_tier: int, mem_tier_size: int,
                               mem_chunk_size: int, ssd_total: int):
    """S4：tier 容量隔离——一层超额不影响另一层；内存层负路径补测。"""
    print("[S4] tier 隔离：内存层超额 → SSD 照常；SSD 超额 → 内存层照常")

    # 内存层超额申请 → NO_MEMORY；随后 SSD 小额探针必须成功
    try:
        client.create_chunk_on_tier(mem_tier_size + mem_chunk_size, mem_tier)
        raise AssertionError("内存层超额申请竟然成功")
    except RuntimeError as e:
        assert "rc=-3" in str(e), f"期望 NO_MEMORY(-3)，实际: {e}"
        print(f"[S4] 内存层超额申请被拒: {e}")
    probe = client.create_chunk(4 * 1024 * 1024, "ssd")
    client.delete_chunk(probe)
    print("[S4] 内存层超额后 SSD 小额申请/释放正常")

    # SSD 超额申请 → NO_MEMORY；随后内存层小额探针必须成功
    try:
        client.create_chunk(ssd_total + mem_chunk_size, "ssd")
        raise AssertionError("SSD 超额申请竟然成功")
    except RuntimeError as e:
        assert "rc=-3" in str(e), f"期望 NO_MEMORY(-3)，实际: {e}"
        print(f"[S4] SSD 超额申请被拒: {e}")
    mprobe = client.create_chunk_on_tier(4 * 1024 * 1024, mem_tier)

    # 顺带补内存层负路径：越界写 / 重复释放
    try:
        client.write_chunk(mprobe, 4 * 1024 * 1024 - 1024, b"\xAA" * 4096)
        raise AssertionError("内存层越界写竟然成功")
    except RuntimeError as e:
        print(f"[S4] 内存层越界写被拒: {e}")
    client.delete_chunk(mprobe)
    try:
        client.delete_chunk(mprobe)
        raise AssertionError("内存层重复释放竟然成功")
    except RuntimeError as e:
        print(f"[S4] 内存层重复释放被拒: {e}")
    print("[S4] tier 隔离 + 内存层负路径 PASS")


# ---------------------------------------------------------------------------
# S5 跨 VM 可见性（共享盘读共享）：A 写完落盘 → B 刷盘后读取
#
# 四级缓存语义：A 的 UMM 写是 memcpy 到 guest 页缓存；fence（本次修正后）
# msync(MS_SYNC) 落盘 → NVMe 写 → QEMU A → 宿主后备文件；B 的 guest 页缓存
# 若无 invalidate 会看到旧数据（mmap 页缓存不会感知另一 VM 的外部写入）。
# writer 进程在 B 验证期间保持存活——否则进程退出 munmap 也会刷盘，
# fence 的落盘语义就无法被单独验证。
# ---------------------------------------------------------------------------

def _s5_expected_digest(tag: str, chunk_id: int, size: int) -> str:
    h = hashlib.sha256()
    for _, piece in chunk_pieces(tag, chunk_id, size):
        h.update(piece)
    return h.hexdigest()


def _s5_read_digest(client, desc, size: int) -> str:
    h = hashlib.sha256()
    npieces = (size + PIECE - 1) // PIECE
    for p in range(npieces):
        n = min(PIECE, size - p * PIECE)
        h.update(client.read_chunk(desc, p * PIECE, n))
    return h.hexdigest()


def _s5_manual_desc(gpa_str: str, chunk_id: int, size: int):
    """按 GPA 手工构造描述符（B 侧未参与分配，经编排侧带外传递 GPA）。
    umm_read/write/invalidate 只依赖 base_gpa + user_size。"""
    from bmpclient.umm_client import ChunkDescriptor
    desc = ChunkDescriptor()
    desc.chunk_id = chunk_id
    desc.base_gpa = int(gpa_str, 16)
    desc.user_size = size
    return desc


def _s5_wait_file(path: str, timeout_s: int, what: str):
    import time
    t0 = time.time()
    while not os.path.exists(path):
        if time.time() - t0 > timeout_s:
            raise RuntimeError(f"[S5] 等待 {what} 超时（{timeout_s}s）：{path}")
        time.sleep(1)


def scenario_s5_writer(client, args):
    """vmA：alloc →（等对端预读）→ 写 pattern → fence 落盘 →
    （等对端验证，进程保持存活）→ 释放。"""
    size = parse_size(args.s5_size)
    s5dir = os.path.expanduser(args.s5_dir)
    desc = client.create_chunk(size, "ssd")
    assert gpa_tier(desc.base_gpa) == UMM_TIER_SSD, \
        f"S5 chunk 不在 SSD tier: gpa=0x{desc.base_gpa:016x}"

    rec = {"chunk_id": desc.chunk_id,
           "gpa": f"0x{desc.base_gpa:016x}", "size": size}
    with open(os.path.join(s5dir, "s5_alloc.json"), "w") as f:
        json.dump(rec, f)
    open(os.path.join(s5dir, "s5_alloc_done"), "w").close()
    print(f"[S5-W] chunk 已分配: id={desc.chunk_id} "
          f"gpa=0x{desc.base_gpa:016x} off=0x{gpa_offset(desc.base_gpa):x}，"
          f"等待对端预读完成", flush=True)

    _s5_wait_file(os.path.join(s5dir, "s5_preread_done"),
                  args.s5_timeout, "对端预读")

    write_chunk_stream(client, desc, args.s5_tag, size)
    client.flush()  # 落盘：CPU 屏障 + 全池 msync(MS_SYNC)
    open(os.path.join(s5dir, "s5_written"), "w").close()
    print(f"[S5-W] {size // 1024 // 1024}MB pattern 写入完成，已 fence 落盘；"
          f"进程保持存活等待对端验证（fence 语义由此被单独验证）", flush=True)

    _s5_wait_file(os.path.join(s5dir, "s5_verify_done"),
                  args.s5_timeout, "对端验证")
    client.delete_chunk(desc)
    print("[S5-W] 对端验证完成，chunk 已释放", flush=True)


def scenario_s5_preread(client, args):
    """vmB：预读目标区域（用旧内容填充本 VM 页缓存——陈旧视图的来源）。"""
    size = parse_size(args.s5_size)
    desc = _s5_manual_desc(args.s5_gpa, args.s5_chunk_id, size)
    d = _s5_read_digest(client, desc, size)
    print(f"[S5-R] 预读完成 digest={d[:16]}…（本 VM 页缓存已填充旧视图）",
          flush=True)


def scenario_s5_verify(client, args):
    """vmB：先不刷盘读（预期可能读到旧数据=负路径证据）→ invalidate 刷盘
    → 再读（必须逐字节匹配写入端 pattern=正路径断言）。"""
    size = parse_size(args.s5_size)
    desc = _s5_manual_desc(args.s5_gpa, args.s5_chunk_id, size)
    expected = _s5_expected_digest(args.s5_tag, args.s5_chunk_id, size)

    stale = _s5_read_digest(client, desc, size)
    if stale != expected:
        print(f"[S5-R] 负路径证据：刷盘前读到陈旧数据（页缓存陷阱复现）"
              f" got={stale[:16]}… expected={expected[:16]}…", flush=True)
    else:
        print("[S5-R] 提示：刷盘前读到的已是新数据（本次缓存未命中旧页，"
              "非失败；负路径未复现但正路径仍可验证）", flush=True)

    client.invalidate_chunk(desc, 0, size)
    print("[S5-R] invalidate 刷盘完成，重新读取", flush=True)
    fresh = _s5_read_digest(client, desc, size)
    assert fresh == expected, \
        f"[S5-R] 刷盘后读仍不匹配: {fresh[:16]}… != {expected[:16]}…"
    print("[S5-R] 刷盘后读回 digest 与写入端 pattern 完全一致 PASS",
          flush=True)


# ================= S6：共享内存窗口（硬件一致，无需 invalidate） =================
# S6 是 S5 的对照实验：共享 DRAM 窗口（virtio-pmem /dev/pmem0 等）经 DAX
# mmap 直达宿主共享物理页，两 vCPU 等同宿主上两个线程，x86 硬件保证
# cacheline 一致——B 不需要任何 invalidate/刷盘动作即可读到 A 的写入。
# 与 S5（guest 页缓存隔着，必须 fence 落盘 + invalidate）形成语义对照。
# 复用 _s5_* 系列通用助手（digest/手工描述符/标记等待）。

def scenario_s6_writer(client, args):
    """vmA：分配内存层 chunk → 写 pattern → flush（内存层 fence=CPU 屏障，
    local load/store 本同步，屏障即排空）→ 进程保活等对端验完才释放。"""
    assert args.mem_tier in (UMM_TIER_DRAM, UMM_TIER_CXL), \
        f"S6 必须跑在内存层 tier（0/1），当前 --mem-tier={args.mem_tier}"
    size = parse_size(args.s6_size)
    s6dir = os.path.expanduser(args.s6_dir)
    desc = client.create_chunk_on_tier(size, args.mem_tier)
    assert gpa_tier(desc.base_gpa) == args.mem_tier, \
        f"S6 chunk 不在内存层: gpa=0x{desc.base_gpa:016x}"

    rec = {"chunk_id": desc.chunk_id,
           "gpa": f"0x{desc.base_gpa:016x}", "size": size,
           "tier": args.mem_tier}
    with open(os.path.join(s6dir, "s6_alloc.json"), "w") as f:
        json.dump(rec, f)
    open(os.path.join(s6dir, "s6_alloc_done"), "w").close()
    print(f"[S6-W] chunk 已分配: id={desc.chunk_id} tier={args.mem_tier} "
          f"gpa=0x{desc.base_gpa:016x}", flush=True)

    write_chunk_stream(client, desc, args.s6_tag, size)
    client.flush()  # 内存层 fence = CPU 屏障（local 存取本同步）
    open(os.path.join(s6dir, "s6_written"), "w").close()
    print(f"[S6-W] {size // 1024 // 1024}MB pattern 写入完成（仅 CPU 屏障，"
          f"无落盘概念）；进程保活等待对端验证", flush=True)

    _s5_wait_file(os.path.join(s6dir, "s6_verify_done"),
                  args.s6_timeout, "对端验证")
    client.delete_chunk(desc)
    print("[S6-W] 对端验证完成，chunk 已释放", flush=True)


def scenario_s6_verify(client, args):
    """vmB：直接读（不执行任何 invalidate/刷盘）——digest 必须与写入端
    逐字节一致。一致 = 硬件一致共享内存成立（x86 cacheline 一致性经
    宿主共享页直接传递给两个 vCPU）。"""
    size = parse_size(args.s6_size)
    desc = _s5_manual_desc(args.s6_gpa, args.s6_chunk_id, size)
    assert gpa_tier(desc.base_gpa) != UMM_TIER_SSD, \
        "S6 目标是内存层共享窗口，不是 SSD tier"

    expected = _s5_expected_digest(args.s6_tag, args.s6_chunk_id, size)
    got = _s5_read_digest(client, desc, size)
    assert got == expected, \
        f"[S6-R] 直接读回不一致: {got[:16]}… != {expected[:16]}…" \
        "（共享窗口未生效？检查两侧 mem_device 是否指向同一窗口、" \
        "以及是否静默回退了 malloc 私有后备）"
    print("[S6-R] 未执行任何 invalidate/刷盘动作，读回 digest 与写入端 "
          "逐字节一致 PASS（硬件一致共享内存；对照 S5 的 fence+invalidate "
          "软件纪律）", flush=True)


def main():
    ap = argparse.ArgumentParser(description="共享盘池实验：单节点客户端")
    ap.add_argument("--meta-addr", required=True)
    ap.add_argument("--mem-addr", required=True)
    ap.add_argument("--node-id", type=int, required=True)
    ap.add_argument("--tag", default=None, help="节点标识（pattern 种子）")
    ap.add_argument("--token", default="")
    ap.add_argument("--devices", required=True,
                    help='"path:size,path:size"（顺序必须与 umms 一致）')
    ap.add_argument("--chunks", type=int, default=4)
    ap.add_argument("--chunk-size", default="2M")
    ap.add_argument("--mem-tier", type=int, default=-1,
                    help="内存层 tier 编号（1=mock CXL 路线A，0=DRAM 路线B）；"
                         "-1 = 不跑混合池场景（默认，保持原行为）")
    ap.add_argument("--mem-chunks", type=int, default=1)
    ap.add_argument("--mem-chunk-size", default="128M")
    ap.add_argument("--mem-tier-size", default="512M",
                    help="服务端内存层总容量（S4 超额测试用）")
    ap.add_argument("--out", default=None, help="结果 JSON 输出路径")
    ap.add_argument("--keep", action="store_true",
                    help="结束时保留 chunk 不释放（供跨阶段复查）")
    # ---- S5（A 写 → fence 落盘 → B invalidate → 读回）专用 ----
    ap.add_argument("--s5-mode", choices=["writer", "preread", "verify"],
                    default=None,
                    help="S5 角色：writer=A 分配+写+fence 落盘（进程保活至 B 验完）；"
                         "preread/verify=B 刷盘前/后读。设置后跳过 S0-S4 常规场景")
    ap.add_argument("--s5-size", default="16M",
                    help="S5 chunk 大小（writer/verify 两侧必须一致）")
    ap.add_argument("--s5-gpa", default=None,
                    help="S5 B 侧：带外获取的 chunk base_gpa（十六进制 0x...）")
    ap.add_argument("--s5-chunk-id", type=int, default=0,
                    help="S5 B 侧：chunk_id（writer 分配后带外告知）")
    ap.add_argument("--s5-tag", default="s5",
                    help="S5 写入 pattern 标签（writer/verify 两侧必须一致）")
    ap.add_argument("--s5-dir", default="~",
                    help="S5 带外同步文件目录（默认 ~，三角色各自本机）")
    ap.add_argument("--s5-timeout", type=int, default=300,
                    help="S5 等待对端标记的超时秒数")
    # ---- 共享内存窗口（Phase 2.5）与 S6 场景 ----
    ap.add_argument("--mem-device", default="",
                    help="内存层后备设备（如 virtio-pmem /dev/pmem0=共享窗口）；"
                         "空 = malloc/mock 私有后备（默认）")
    ap.add_argument("--s6-mode", choices=["writer", "verify"], default=None,
                    help="S6 角色（共享内存窗口对照实验）：writer=A 写+屏障；"
                         "verify=B 直接读（不 invalidate）。设置后跳过 S0-S4")
    ap.add_argument("--s6-size", default="16M", help="S6 chunk 大小")
    ap.add_argument("--s6-gpa", default=None,
                    help="S6 B 侧：带外获取的 chunk base_gpa（十六进制 0x...）")
    ap.add_argument("--s6-chunk-id", type=int, default=0,
                    help="S6 B 侧：chunk_id（writer 分配后带外告知）")
    ap.add_argument("--s6-tag", default="s6", help="S6 pattern 标签")
    ap.add_argument("--s6-dir", default="~", help="S6 带外同步文件目录")
    ap.add_argument("--s6-timeout", type=int, default=300,
                    help="S6 等待对端标记的超时秒数")
    args = ap.parse_args()

    tag = args.tag or f"node{args.node_id}"
    devs = parse_devices(args.devices)
    chunk_size = parse_size(args.chunk_size)
    total = sum(sz for _, sz in devs)
    mem_chunk_size = parse_size(args.mem_chunk_size)
    mem_tier_size = parse_size(args.mem_tier_size)
    hybrid = args.mem_tier >= 0

    print(f"[setup] tag={tag} node_id={args.node_id} "
          f"devices={[(p, sz // (1 << 20)) for p, sz in devs]}MB "
          f"pool_total={total // (1 << 20)}MB")
    print(f"[setup] 共享盘模型：不设 peer_nodes，remote transport 不挂载")

    from bmpclient.client import UMMServiceClient
    client = UMMServiceClient(
        meta_addr=args.meta_addr, mem_addr=args.mem_addr,
        node_id=args.node_id, tier_aware=True,
        ssd_devices=devs, rpc_token=args.token,
        # 混合池：客户端本地内存数据面（malloc 后备）须装得下本进程
        # 全部内存层 chunk——直接与服务端内存层容量对齐，简单且够用。
        memory_size=mem_tier_size if hybrid else 0,
        # 路线B（--mem-tier 0）：本地数据面注册真 DRAM tier；路线A 保持默认
        local_mem_as_dram=1 if args.mem_tier == 0 else 0,
        # 共享内存窗口（Phase 2.5）：内存层后备设备（空=私有 malloc/mock）
        mem_device=args.mem_device)

    # S5 模式：独立角色流程，不跑 S0-S4
    if args.s5_mode:
        if args.s5_mode in ("preread", "verify") and not args.s5_gpa:
            ap.error("--s5-mode preread/verify 需要 --s5-gpa（带外获取）")
        if args.s5_mode == "writer":
            scenario_s5_writer(client, args)
        elif args.s5_mode == "preread":
            scenario_s5_preread(client, args)
        else:
            scenario_s5_verify(client, args)
        client.close()
        return

    # S6 模式：共享内存窗口对照实验，不跑 S0-S4
    if args.s6_mode:
        if not args.mem_device:
            ap.error("S6 需要 --mem-device 指向共享内存窗口（如 /dev/pmem0）")
        if args.mem_tier not in (0, 1):
            ap.error("S6 需要 --mem-tier 0（DRAM）或 1（CXL）")
        if args.s6_mode == "verify" and not args.s6_gpa:
            ap.error("--s6-mode verify 需要 --s6-gpa（带外获取）")
        if args.s6_mode == "writer":
            scenario_s6_writer(client, args)
        else:
            scenario_s6_verify(client, args)
        client.close()
        return

    # S0：拓扑可见性（ummD 全局视图，信息性）
    try:
        devs_topo = client.get_device_list()
        ssd_topo = [d for d in devs_topo if d["tier"] == UMM_TIER_SSD]
        print(f"[S0] ummD 拓扑中 SSD 资源 {len(ssd_topo)} 项: "
              f"{[d['device_path'] for d in ssd_topo]}")
    except Exception as e:
        print(f"[S0] 拓扑查询失败（不影响数据面）: {e}")

    descs = scenario_s1_rw(client, tag, args.chunks, chunk_size)
    mem_descs = []
    if hybrid:
        mem_descs = scenario_s1m_mem_rw(client, tag, args.mem_tier,
                                        args.mem_chunks, mem_chunk_size)
    scenario_s3_negative(client, total, chunk_size)
    if hybrid:
        scenario_s4_tier_isolation(client, args.mem_tier, mem_tier_size,
                                   mem_chunk_size, total)

    def chunk_rec(ds, tier):
        return [
            {
                "chunk_id": d.chunk_id,
                "gpa": f"0x{d.base_gpa:016x}",
                "gpa_node": gpa_node(d.base_gpa),
                "tier": tier,
                "offset": gpa_offset(d.base_gpa),
                "size": d.user_size,
            }
            for d in ds
        ]

    result = {
        "tag": tag,
        "node_id": args.node_id,
        "devices": [{"path": p, "size": sz} for p, sz in devs],
        "chunks": chunk_rec(descs, UMM_TIER_SSD),
        "mem_chunks": chunk_rec(mem_descs, args.mem_tier) if hybrid else [],
    }
    out = args.out or f"/tmp/umm_shared_pool_{tag}.json"
    with open(out, "w") as f:
        json.dump(result, f, indent=2)
    print(f"[S2] 结果已写出: {out}（{len(descs)} SSD + "
          f"{len(mem_descs)} 内存层 chunk offset 记录）")

    if args.keep:
        print("[cleanup] --keep：chunk 保留不释放")
    else:
        for d in descs + mem_descs:
            client.delete_chunk(d)
        print(f"[cleanup] {len(descs) + len(mem_descs)} 个 chunk 已释放")
    client.close()

    print(f"\n=== {tag}: 共享池实验单节点流程全部 PASS ===")


if __name__ == "__main__":
    main()
