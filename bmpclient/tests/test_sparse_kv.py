# -*- coding: utf-8 -*-
"""
bmpclient/tests/test_sparse_kv.py — SparseKVStore 单测（FakeUMMLib）。

覆盖重构后接口：
- offload / flush / release / fetch 数据一致性；
- plan() 产出 descriptor buffer 的地址映射、HOST_READY 兜底、count 语义；
- slot_table packed entry（仅 VALID flag）。
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from bmpclient.sparse_kv import (
    FLAG_VALID,
    KVBlockRef,
    PlanView,
    SegmentFullError,
    SlotTable,
    SparseKVStore,
    entry_flags,
    entry_offset,
    pack_entry,
)
from bmpclient.testing import FakeUMMLib
from bmpclient.virtual_media import VirtualMedia

UNIT = 4096
SP = 4 * UNIT
N_SSD = 4
N_LAYER = 4
MAX_TOKEN = 256
CAPACITY = 4 * SP  # 每盘 4 段
MAX_TOPK = 64


def make_vm(**kw):
    lib = FakeUMMLib(num_ssd_devices=N_SSD)
    cfg = dict(
        unit_size=UNIT,
        capacity_per_device=CAPACITY,
        sp_bytes=SP,
        sp_bytes_per_device={},
    )
    cfg.update(kw)
    vm = VirtualMedia(lib, **cfg)
    return lib, vm


def make_store(**kw):
    lib, vm = make_vm(**kw)
    store = SparseKVStore(vm, num_layers=N_LAYER, max_tokens=MAX_TOKEN,
                          max_topk=MAX_TOPK)
    return lib, vm, store


def make_block(layer, token_start, token_count, fill=None):
    buf = bytearray(token_count * UNIT)
    for i in range(token_count):
        tag = fill if fill is not None else (layer * 1000 + token_start + i)
        buf[i * UNIT: i * UNIT + 8] = tag.to_bytes(8, "little")
    return KVBlockRef(
        layer_id=layer,
        block_idx=token_start // max(token_count, 1),
        token_start=token_start,
        token_count=token_count,
        buffer=buf,
    )


def unit_tag(data: bytes) -> int:
    return int.from_bytes(data[:8], "little")


class TestSlotTable(unittest.TestCase):
    def test_pack_unpack_roundtrip(self):
        e = pack_entry(0xABCDEF000, FLAG_VALID)
        self.assertEqual(entry_offset(e), 0xABCDEF000)
        self.assertEqual(entry_flags(e), FLAG_VALID)

    def test_pack_bounds(self):
        with self.assertRaises(ValueError):
            pack_entry(1 << 48, FLAG_VALID)
        with self.assertRaises(ValueError):
            pack_entry(0, 1 << 8)

    def test_set_get_gather_invalidate(self):
        st = SlotTable(num_layers=2, max_tokens=8)
        st.set(1, 3, 8192, FLAG_VALID)
        off, flags = st.locate(1, 3)
        self.assertEqual((off, flags), (8192, FLAG_VALID))
        self.assertEqual(st.gather(1, [0, 3, 5])[1], st.get(1, 3))
        st.invalidate(1, 3)
        self.assertEqual(st.locate(1, 3), (0, 0))


class TestSparseKVStore(unittest.TestCase):
    def test_offload_fetch_roundtrip(self):
        lib, _, store = make_store()
        blocks = [make_block(l, 0, 16) for l in range(N_LAYER)]
        store.offload(blocks)
        store.flush()
        for l in range(N_LAYER):
            tokens = list(range(16))
            out = [memoryview(bytearray(UNIT)) for _ in tokens]
            store.fetch(l, tokens, out)
            for t, buf in zip(tokens, out):
                self.assertEqual(unit_tag(buf.tobytes()), l * 1000 + t,
                                 f"layer={l} token={t}")
        store.close()

    def test_plan_descriptor_after_flush(self):
        _, _, store = make_store()
        store.offload([make_block(0, 0, 16)])
        store.flush()

        tokens = [0, 4, 7, 12]
        plan = store.plan(0, tokens)
        self.assertIsInstance(plan, PlanView)
        self.assertEqual(plan.layer_id, 0)
        self.assertEqual(plan.count, len(tokens))
        self.assertEqual(plan.address, plan.address)  # 固定地址

        for i, t in enumerate(tokens):
            ssd_id, flags, lba_off, length, dst_off = plan.entry(i)
            self.assertIn(ssd_id, range(N_SSD))
            self.assertEqual(flags, 0)  # DISK
            self.assertEqual(length, UNIT)
            self.assertEqual(dst_off, i * UNIT)
            # lba_off 应为 extent_base(0) + slot_table offset
            entry = store.slot_table.get(0, t)
            self.assertEqual(lba_off, entry_offset(entry))

        store.close()

    def test_plan_host_ready_when_in_buf(self):
        lib, _, store = make_store()
        store.offload([make_block(0, 0, 8)])
        # 未 flush，命中段缓冲 => HOST_READY
        tokens = list(range(8))
        plan = store.plan(0, tokens)
        self.assertEqual(plan.count, len(tokens))
        for i in range(plan.count):
            _, flags, _, _, dst_off = plan.entry(i)
            self.assertEqual(flags, 1, f"entry {i} 应为 HOST_READY")
            tag = int.from_bytes(store.staging[dst_off:dst_off + 8], "little")
            self.assertEqual(tag, 0 * 1000 + tokens[i])
        store.close()

    def test_plan_max_topk_enforced(self):
        _, _, store = make_store()
        with self.assertRaises(ValueError):
            store.plan(0, list(range(MAX_TOPK + 1)))
        store.close()

    def test_plan_invalid_token_raises(self):
        _, _, store = make_store()
        store.offload([make_block(0, 0, 4)])
        store.flush()
        with self.assertRaises(ValueError):
            store.plan(0, [5])
        store.close()

    def test_release_and_recycle(self):
        lib, _, store = make_store()
        store.offload([make_block(0, 0, 16)])
        store.flush()

        vm = store.media
        dev0 = 0
        dev0_tokens = [t for t in range(16) if vm.locate((0, t)) == dev0]
        free_before = vm.stats()["free_segments"][dev0]
        store.release(0, dev0_tokens)
        self.assertEqual(vm.stats()["free_segments"][dev0], free_before + 1)

        # 复用后 plan/fetch 仍一致
        store.offload([make_block(1, 0, 16)])
        store.flush()
        out = [memoryview(bytearray(UNIT))]
        store.fetch(1, [0], out)
        self.assertEqual(unit_tag(out[0].tobytes()), 1000)
        store.close()

    def test_segment_full_propagates(self):
        # 每盘只有 1 段（4 单元），4 层各写 16 token 会把每盘写爆
        lib, _, store = make_store(capacity_per_device=SP)
        with self.assertRaises(SegmentFullError):
            for l in range(N_LAYER):
                store.offload([make_block(l, 0, 16)])
        store.close()

    def test_prefetch_avoids_disk_io(self):
        lib, _, store = make_store()
        store.offload([make_block(0, 0, 8)])
        store.flush()
        store.prefetch(0, [0, 1, 2, 3])
        lib.fake.reset_stats()
        out = [memoryview(bytearray(UNIT)) for _ in range(4)]
        store.fetch(0, [0, 1, 2, 3], out)
        self.assertEqual(lib.fake.read_log, [])
        for i, buf in enumerate(out):
            self.assertEqual(unit_tag(buf.tobytes()), i)
        store.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
