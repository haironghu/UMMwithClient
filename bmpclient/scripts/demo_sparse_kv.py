# -*- coding: utf-8 -*-
"""
bmpclient/scripts/demo_sparse_kv.py — 稀疏注意力 KV cache 多 SSD 卸载演示。

用 FakeUMMLib（内存模拟 4 块 SSD）演示合并后的新栈：
  - VirtualMedia：盘感知 + 位置哈希打散 + 段聚合写；
  - SparseKVStore：vllm block 语义 + slot_table + plan() 地址映射输出。

运行（仓库根目录）：
    python3 bmpclient/scripts/demo_sparse_kv.py
"""

import os
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from bmpclient.sparse_kv import KVBlockRef, SparseKVStore
from bmpclient.testing import FakeUMMLib
from bmpclient.virtual_media import VirtualMedia

N_SSD = 4
N_LAYER = 4
MAX_TOKEN = 256
UNIT = 4096
SP = 4 * UNIT
BLOCK = 16
N_BLOCK = 16
CAPACITY = 64 * SP
MAX_TOPK = 64


def tag_of(layer: int, token: int) -> int:
    return layer * 100000 + token


def make_blocks():
    """构造全量 KV：N_LAYER 层 × MAX_TOKEN 个 token 的 vllm block。"""
    blocks = []
    for l in range(N_LAYER):
        for b in range(N_BLOCK):
            start = b * BLOCK
            buf = bytearray(BLOCK * UNIT)
            for i in range(BLOCK):
                buf[i * UNIT: i * UNIT + 8] = tag_of(l, start + i).to_bytes(8, "little")
            blocks.append(KVBlockRef(
                layer_id=l, block_idx=b, token_start=start,
                token_count=BLOCK, buffer=buf,
            ))
    return blocks


def topk_of_layer(layer: int, k: int = 48) -> list:
    """模拟稀疏 topk：局部聚集 + 层间相似。"""
    base_clusters = [(20, 44), (100, 112), (200, 216)]
    tokens = []
    for lo, hi in base_clusters:
        tokens.extend(range(lo + layer % 3, hi, 2))
    tokens.extend([(layer * 7 + i * 31) % MAX_TOKEN for i in range(k - len(tokens))])
    return sorted(set(tokens))


def check(store, layer, tokens):
    """fetch 并校验数据一致性。"""
    out = [memoryview(bytearray(UNIT)) for _ in tokens]
    store.fetch(layer, tokens, out)
    for t, buf in zip(tokens, out):
        got = int.from_bytes(buf.tobytes()[:8], "little")
        assert got == tag_of(layer, t), f"数据不符: layer={layer} token={t} got={got}"


def main():
    lib = FakeUMMLib(num_ssd_devices=N_SSD)
    vm = VirtualMedia(
        lib,
        unit_size=UNIT,
        capacity_per_device=CAPACITY,
        sp_bytes=SP,
        sp_bytes_per_device={},
    )
    store = SparseKVStore(vm, num_layers=N_LAYER, max_tokens=MAX_TOKEN,
                          max_topk=MAX_TOPK)

    print("=" * 72)
    print("场景 W：prefill 全量 offload（4 层 × 256 token × 4KB = 4MB）")
    print("=" * 72)
    store.offload(make_blocks())

    # 打散效果：盘号分布（哈希直接算，零元数据）
    dist = Counter()
    for t in range(MAX_TOKEN):
        for l in range(N_LAYER):
            dist[vm.locate((l, t))] += 1
    print(f"位置哈希分盘分布（共 {MAX_TOKEN * N_LAYER} 单元）: "
          + ", ".join(f"SSD{d}={c}" for d, c in sorted(dist.items())))
    cover = {vm.locate((0, t)) for t in range(37, 37 + N_SSD)}
    print(f"性质一抽查：layer 0 连续 token [37,41) 落盘 {sorted(cover)}（覆盖全部 {N_SSD} 盘）")

    store.flush()
    writes = lib.fake.write_log
    per_dev = Counter(lib.fake._chunk_device[cid] for cid, _, _ in writes)
    sizes = Counter(size for _, _, size in writes)
    print(f"段聚合写：共 {len(writes)} 次盘 IO（每盘 {dict(per_dev)} 次），"
          f"IO 尺寸分布 {dict(sizes)} 字节 —— 全部为段粒度连续大 IO")
    total_units = MAX_TOKEN * N_LAYER
    print(f"对照：不聚合则需 {total_units} 次 {UNIT}B 小 IO；"
          f"聚合后 {len(writes)} 次（缩减 {total_units // len(writes)} 倍）")

    print()
    print("=" * 72)
    print("场景 R：decode 稀疏 topk 地址规划 + CPU 兜底读")
    print("=" * 72)
    lib.fake.reset_stats()
    total_reads = 0
    for l in range(N_LAYER):
        tokens = topk_of_layer(l)
        # plan() 产出 GPU 直通算子可用的地址映射表
        plan = store.plan(l, tokens)
        print(f"layer {l}: topk={len(tokens):3d} 个 token → descriptor count={plan.count}, "
              f"buffer 地址=0x{plan.address:x}")
        # 打印第一条 entry 样例
        ssd_id, flags, lba_off, length, dst_off = plan.entry(0)
        print(f"  第 0 条 entry: ssd_id={ssd_id}, flags={'HOST_READY' if flags else 'DISK'}, "
              f"lba_offset={lba_off}, length={length}, dst_offset={dst_off}")

        before = len(lib.fake.read_log)
        check(store, l, tokens)
        reads = lib.fake.read_log[before:]
        devs_hit = {lib.fake._chunk_device[cid] for cid, _, _ in reads}
        total_reads += len(reads)
        print(f"  CPU 兜底 fetch：{len(reads)} 次盘读 IO，命中 {len(devs_hit)} 块盘并行")
    print(f"合计：{total_reads} 次盘读 IO；数据全部校验一致 ✓")

    print()
    print("=" * 72)
    print("场景 F：释放 + 段回收复用")
    print("=" * 72)
    free0 = [vm.stats()["free_segments"][d] for d in range(N_SSD)]
    for l in range(N_LAYER):
        store.release(l, list(range(0, MAX_TOKEN, 2)))
    free1 = [vm.stats()["free_segments"][d] for d in range(N_SSD)]
    print(f"释放一半 token：各盘空闲段 {free0} → {free1}")
    store.offload(make_blocks()[:N_BLOCK])
    store.flush()
    check(store, 0, topk_of_layer(0))
    print("回收段复用后重新 offload + fetch，数据一致 ✓")

    store.close()
    print()
    print("演示完成。")


if __name__ == "__main__":
    main()
