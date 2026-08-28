# -*- coding: utf-8 -*-
"""
bmpclient/tests/test_virtual_media.py — VirtualMedia（稀疏 KV 专用介质层）单测。

全部基于 FakeUMMLib，无需启动真实 UMM 服务。
"""

import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from bmpclient.testing import FakeUMMLib
from bmpclient.virtual_media import VirtualMedia, SegmentFullError
from bmpclient.virtual_media_config import load_config
from bmpclient.virtual_media_strategy import PlacementStrategy, PositionHashStrategy

UNIT = 4096
SP = 4 * UNIT
N_SSD = 4
CAPACITY = 4 * SP  # 每盘 4 段，每段 4 单元


def make_unit(layer: int, token: int) -> bytes:
    """生成可辨识的 4KB 单元数据。"""
    tag = (layer << 20) | token
    return tag.to_bytes(8, "little").ljust(UNIT, b"\x00")


def unit_tag(data: bytes) -> int:
    return int.from_bytes(data[:8], "little")


class TestVirtualMedia(unittest.TestCase):
    def _make(self, **kw):
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

    def test_init_properties_and_default_strategy(self):
        _, vm = self._make()
        self.assertEqual(vm.unit_size, UNIT)
        self.assertEqual(vm.device_count, N_SSD)
        self.assertIsInstance(vm.strategy, PositionHashStrategy)
        self.assertEqual(vm.strategy.name, "positionhash")
        vm.close()

    def test_locate_basic(self):
        _, vm = self._make()
        for l in range(4):
            for t in (0, 1, 37, 255):
                d = vm.locate((l, t))
                self.assertIn(d, range(N_SSD))
        vm.close()

    def test_position_hash_properties(self):
        _, vm = self._make()
        strat = vm.strategy
        # 确定性
        for l in range(4):
            for t in (0, 1, 37, 255):
                self.assertEqual(strat.locate((l, t)), strat.locate((l, t)))
        # 性质一：连续 N_SSD 个 token 覆盖全部盘
        for l in range(4):
            for start in (0, 5, 100, 200):
                devs = {strat.locate((l, t)) for t in range(start, start + N_SSD)}
                self.assertEqual(devs, set(range(N_SSD)), f"layer={l} start={start}")
        # 性质二：同一 token 连续 N_SSD 层覆盖全部盘
        for t in (0, 7, 128, 200):
            devs = {strat.locate((l, t)) for l in range(N_SSD)}
            self.assertEqual(devs, set(range(N_SSD)), f"token={t}")
        vm.close()

    def test_locate_batch_matches_single(self):
        _, vm = self._make()
        keys = [(l, t) for l in range(4) for t in range(16)]
        batch = vm.locate_batch(keys)
        self.assertEqual(batch, [vm.locate(k) for k in keys])
        vm.close()

    def test_write_returns_offset_and_flush_segment_io(self):
        lib, vm = self._make()
        for t in range(16):
            off = vm.write((0, t), make_unit(0, t))
            self.assertEqual(off % UNIT, 0)
            self.assertGreaterEqual(off, 0)

        # flush 前无盘写（全部在段缓冲）
        self.assertEqual(lib.fake.write_log, [])
        vm.flush()
        # 4 盘各 1 段 => 4 次段大小连续 IO
        self.assertEqual(len(lib.fake.write_log), N_SSD)
        for _, off, size in lib.fake.write_log:
            self.assertEqual(size, SP)
            self.assertEqual(off % SP, 0)
        vm.close()

    def test_read_batch_roundtrip(self):
        lib, vm = self._make()
        keys = [(l, t) for l in range(2) for t in range(16)]
        offsets = {}
        for k in keys:
            offsets[k] = vm.write(k, make_unit(*k))
        vm.flush()

        items = [(vm.locate(k), offsets[k]) for k in keys]
        outs = [memoryview(bytearray(UNIT)) for _ in keys]
        vm.read_batch(items, outs)
        for k, out in zip(keys, outs):
            self.assertEqual(unit_tag(out.tobytes()), (k[0] << 20) | k[1], k)
        vm.close()

    def test_in_buf_read_before_flush_no_disk_io(self):
        lib, vm = self._make()
        keys = [(0, t) for t in range(8)]
        offsets = {k: vm.write(k, make_unit(*k)) for k in keys}
        lib.fake.reset_stats()

        items = [(vm.locate(k), offsets[k]) for k in keys]
        outs = [memoryview(bytearray(UNIT)) for _ in keys]
        vm.read_batch(items, outs)
        self.assertEqual(lib.fake.read_log, [])
        for k, out in zip(keys, outs):
            self.assertEqual(unit_tag(out.tobytes()), (k[0] << 20) | k[1])
        vm.close()

    def test_release_recycles_segment(self):
        lib, vm = self._make()
        keys = [(0, t) for t in range(16)]
        offsets = {}
        for k in keys:
            offsets[k] = vm.write(k, make_unit(*k))
        vm.flush()

        # 释放 device 0 上所有单元
        dev0 = 0
        dev0_keys = [k for k in keys if vm.locate(k) == dev0]
        free_before = vm.stats()["free_segments"][dev0]
        for k in dev0_keys:
            vm.release(dev0, offsets[k])
        self.assertEqual(vm.stats()["free_segments"][dev0], free_before + 1)

        # 复用回收段：再写入 device 0 会命中同一段，不抛满盘
        reused = 0
        for t in range(256):
            k = (1, t)
            if vm.locate(k) == dev0:
                offsets[k] = vm.write(k, make_unit(*k))
                reused += 1
                if reused == len(dev0_keys):
                    break
        vm.flush()
        # 只要复用成功，device 0 空闲段数应回到之前
        self.assertEqual(vm.stats()["free_segments"][dev0], free_before)
        vm.close()

    def test_segment_full_raises(self):
        # 每盘只有 1 段（4 单元）
        lib, vm = self._make(capacity_per_device=SP)
        dev0 = 0
        written = 0
        with self.assertRaises(SegmentFullError):
            for t in range(256):
                if vm.locate((0, t)) == dev0:
                    vm.write((0, t), make_unit(0, t))
                    written += 1
                    # 第 5 次同盘写入时，第 4 个单元所在的段被 flush 后无空闲段
        self.assertGreaterEqual(written, 4)
        vm.close()

    def test_per_device_segment_size(self):
        lib, vm = self._make(sp_bytes_per_device={0: 2 * SP, 2: SP // 2})
        keys = [(0, t) for t in range(16)]
        for k in keys:
            vm.write(k, make_unit(*k))
        vm.flush()

        # 数据一致性不受影响
        offsets = {k: vm.write(k, make_unit(1, k[1])) for k in keys}  # 会分配新段
        vm.flush()
        for k in keys:
            items = [(vm.locate(k), offsets[k])]
            outs = [memoryview(bytearray(UNIT))]
            vm.read_batch(items, outs)
            self.assertEqual(unit_tag(outs[0].tobytes()), (1 << 20) | k[1], k)

        # device 2 的段大小为 SP/2；device 0 为 2*SP
        for cid, off, size in lib.fake.write_log:
            dev = lib.fake._chunk_device[cid]
            if dev == 2:
                self.assertEqual(size, SP // 2)
            elif dev == 0:
                self.assertEqual(size, 2 * SP)
        vm.close()

    def test_close_releases_chunks(self):
        lib, vm = self._make()
        vm.write((0, 0), make_unit(0, 0))
        vm.close()
        # close 后底层 chunk 已释放，再操作应失败
        with self.assertRaises(Exception):
            vm.write((0, 1), make_unit(0, 1))

    def test_thread_safety_write(self):
        lib, vm = self._make()
        errors = []
        lock = threading.Lock()

        def worker(layer):
            try:
                for t in range(16):
                    vm.write((layer, t), make_unit(layer, t))
            except Exception as e:
                with lock:
                    errors.append(e)

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        self.assertEqual(errors, [])
        vm.flush()
        # 每个 thread 写了 16 单元，共 64 单元；平均每盘 16 单元 = 4 段
        self.assertEqual(len(lib.fake.write_log), N_SSD * 4)
        vm.close()

    def test_explicit_strategy_instance(self):
        class ReverseStrategy(PlacementStrategy):
            def locate(self, key):
                return (self.num_devices - 1) - (key[1] % self.num_devices)

        _, vm = self._make(strategy=ReverseStrategy(N_SSD, {}))
        self.assertIsInstance(vm.strategy, ReverseStrategy)
        for t in range(8):
            self.assertEqual(vm.locate((0, t)), (N_SSD - 1) - (t % N_SSD))
        vm.close()

    def test_config_file_strategy(self):
        import tempfile
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            f.write('{"step_idx": 19, "step_layer": 29, "prime": 2229299}')
            config_path = f.name
        try:
            cfg = load_config(config_path)
            self.assertEqual(cfg["step_idx"], 19)
            lib, vm = self._make(config_path=config_path)
            self.assertIsInstance(vm.strategy, PositionHashStrategy)
            # step_idx=19 改变了哈希结果，但仍应在合法范围
            for t in range(32):
                self.assertIn(vm.locate((0, t)), range(N_SSD))
            vm.close()
        finally:
            os.unlink(config_path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
