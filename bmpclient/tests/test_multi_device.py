import contextlib
import ctypes
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from bmpclient.bench.layout_replay import parser, run, geometry, DataMismatchError
from bmpclient.bench.single_ssd import DirectPool
from bmpclient.sparse_kv import SparseKVStore
from bmpclient.bench.multi_device import OriginalLayerHash, hash_capacities, load_devices, LayerPlacement, layer_capacities
from bmpclient.bench.single_ssd import load_trace
from bmpclient.virtual_media_strategy import PositionHashStrategy
from bmpclient.testing import FakeUMMLib
from bmpclient.virtual_media import VirtualMedia


class TestMultiDevice(unittest.TestCase):
    def test_layer_placement_boundaries_and_capacity(self):
        for mode in ('range', 'stripe'):
            for length in (1, 7, 8, 9, 31, 32, 33, 65):
                with self.subTest(mode=mode, length=length):
                    lengths = {('a', 19): length, ('a', 31): length}
                    strategy = LayerPlacement(8, [19, 31], 'a', lengths, mode, 4)
                    caps, rows = layer_capacities(12288, lengths, 8, mode, 4)
                    counts = [0] * 8
                    assignments = [strategy.locate((0, t)) for t in range(length)]
                    for d in assignments:
                        counts[d] += 1
                    self.assertEqual(rows['a'], [n * 2 for n in counts])
                    self.assertEqual(assignments, [strategy.locate((1, t)) for t in range(length)])
                    if mode == 'range':
                        self.assertEqual(assignments, sorted(assignments))
                        self.assertLessEqual(max(counts) - min(counts), 1)
                    else:
                        self.assertEqual(assignments, [(t // 4) % 8 for t in range(length)])
                    self.assertTrue(all(n % 12288 == 0 for n in caps['a']))

    @unittest.skipUnless(os.environ.get('UMM_TEST_DIRECT') == '1', 'opt-in temporary file integration')
    def test_layer_placement_native_replay(self):
        for mode in ('range', 'stripe'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix='umm-placement-') as directory:
                root = Path(directory)
                trace, config = self.prepare(root)
                argv = ['--trace', str(trace), '--devices-config', str(config),
                    '--output', str(root / 'out.json'), '--experiment', 'layout-concurrency',
                    '--segments', '12K', '--worker-sweep', '1', '4', '--repeats', '1',
                    '--placement', mode, '--stripe-bytes', '16K']
                result = run(parser().parse_args(argv))
                self.assertEqual(result['status'], 'complete')
                self.assertEqual(len(result['trials']), 4)
                self.assertIsNone(result['hash_strategy'])
                for trial in result['trials']:
                    active = [d['device_idx'] for d in trial['io']['devices'] if d['read_calls']]
                    self.assertEqual(active, list(range(8)))
                    self.assertEqual(trial['placement'], mode)
                    self.assertEqual(trial['io']['read_calls'], sum(b['topk'] for b in trial['batches']))

    @unittest.skipUnless(os.environ.get('UMM_TEST_DIRECT') == '1', 'opt-in temporary file integration')
    def test_exact_superpage_boundary_readback_and_write_latency(self):
        with tempfile.TemporaryDirectory(prefix='umm-superpage-') as directory:
            root = Path(directory)
            trace = root / 'trace.jsonl'
            trace.write_text(json.dumps(dict(request_id='r', step_id=0, layer_id=26,
                context_length=6505, topk_token_indices=[0, 6503, 6504, 0])) + '\n')
            args = parser().parse_args(['--trace', str(trace), '--file', str(root / 'data'),
                '--output', str(root / 'out.json'), '--experiment', 'layout-concurrency',
                '--segments', '26640384', '--worker-sweep', '1', '--repeats', '1'])
            result = run(args)
            self.assertEqual(result['status'], 'complete')
            for t in result['trials']:
                self.assertEqual(t['segment_bytes'], 26640384)
                self.assertEqual(t['write_call_count'], 2)
                self.assertEqual(t['io']['write_bytes'], 2 * 26640384)
                self.assertGreater(t['write_call_us']['p50'], 0)
                self.assertGreater(t['write_call_total_ms'], 0)

    @unittest.skipUnless(os.environ.get('UMM_TEST_DIRECT') == '1', 'opt-in temporary file integration')
    def test_write_watch_detects_missing_write_and_later_overwrite(self):
        original_write, original_sync = DirectPool.write, DirectPool.sync

        def drop_write(pool, off, size, ptr):
            if off != 0:
                original_write(pool, off, size, ptr)

        def overwrite_after_sync(pool):
            original_sync(pool)
            zero = bytearray(4096)
            owner = (ctypes.c_char * 4096).from_buffer(zero)
            original_write(pool, 0, 4096, ctypes.addressof(owner))

        for mode in ('healthy', 'dropped', 'overwritten'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix='umm-watch-') as directory:
                root = Path(directory)
                trace, config = self.prepare(root)
                output = root / 'out.json'
                args = parser().parse_args(['--trace', str(trace), '--devices-config', str(config),
                    '--output', str(output), '--experiment', 'layout-concurrency',
                    '--segments', '16K', '--worker-sweep', '1', '--repeats', '1',
                    '--watch-device-offset', '0', '4096'])
                if mode == 'healthy':
                    result = run(args)
                    self.assertTrue(all(e['status'] == 'matched' for e in result['write_watch']))
                else:
                    method, replacement = ('write', drop_write) if mode == 'dropped' else ('sync', overwrite_after_sync)
                    with patch.object(DirectPool, method, replacement), self.assertRaises(DataMismatchError):
                        run(args)
                    result = json.loads(output.read_text())
                    failure = result['failure']
                    self.assertTrue(failure['actual_all_zero'])
                    self.assertFalse(failure['expected_all_zero'])
                    self.assertEqual(failure['stage'], 'immediately_after_write' if mode == 'dropped' else 'after_snapshot_sync')

    @unittest.skipUnless(os.environ.get('UMM_TEST_DIRECT') == '1', 'opt-in temporary file integration')
    def test_mismatch_records_address_and_independent_native_retry(self):
        original_fetch, original_read = SparseKVStore.fetch, DirectPool.read

        def corrupt_fetch(store, layer, tokens, outs):
            original_fetch(store, layer, tokens, outs)
            outs[0][0] ^= 1

        def corrupt_native(pool, off, size, ptr):
            original_read(pool, off, size, ptr)
            ctypes.c_ubyte.from_address(ptr).value ^= 1

        for native in (False, True):
            with self.subTest(native=native), tempfile.TemporaryDirectory(prefix='umm-mismatch-') as directory:
                root = Path(directory)
                trace, config = self.prepare(root)
                output = root / 'out.json'
                args = parser().parse_args(['--trace', str(trace), '--devices-config', str(config),
                    '--output', str(output), '--experiment', 'layout-concurrency',
                    '--segments', '16K', '--worker-sweep', '1', '--repeats', '1'])
                patcher = patch.object(DirectPool, 'read', corrupt_native) if native else patch.object(SparseKVStore, 'fetch', corrupt_fetch)
                with patcher, self.assertRaises(DataMismatchError):
                    run(args)
                result = json.loads(output.read_text())
                failure = result['failure']
                self.assertEqual(result['status'], 'failed')
                self.assertEqual(result['active_trial']['workers'], 1)
                self.assertEqual(failure['first_mismatch_byte'], 0)
                self.assertEqual(failure['native_retry']['matches_expected'], not native)
                self.assertEqual(failure['native_retry']['matches_first'], native)
                device = result['devices'][failure['device_idx']]
                self.assertEqual(failure['device_path'], device['target'])
                self.assertGreaterEqual(failure['device_byte_offset'], device['window_offset'])
                self.assertLess(failure['device_byte_offset'], device['possible_write_range'][1])

    @unittest.skipUnless(os.environ.get('UMM_TEST_DIRECT') == '1', 'opt-in temporary file integration')
    def test_layout_concurrency_fixed_large_writes_no_merge(self):
        with tempfile.TemporaryDirectory(prefix='umm-layout-sweep-') as directory:
            root = Path(directory)
            trace, config = self.prepare(root)
            args = parser().parse_args(['--trace', str(trace), '--devices-config', str(config),
                '--output', str(root / 'out.json'), '--experiment', 'layout-concurrency',
                '--segments', '16K', '--worker-sweep', '1', '4', '--repeats', '1'])
            result = run(args)
            self.assertFalse(result['merge'])
            self.assertEqual(result['segments'], [16384])
            self.assertEqual(len(result['trials']), 4)
            self.assertEqual(len(result['comparisons']), 4)
            hashes = {}
            for t in result['trials']:
                self.assertEqual(t['segment_bytes'], 16384)
                self.assertEqual(t['io']['read_calls'], sum(b['topk'] for b in t['batches']))
                self.assertEqual(t['io']['write_bytes'], t['io']['write_calls'] * 16384)
                if t['layout'] in hashes:
                    self.assertEqual(hashes[t['layout']], t['mapping_sha256'])
                hashes[t['layout']] = t['mapping_sha256']

    def test_layout_concurrency_validation_and_dry_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace, config = self.prepare(root, 'device')
            entries = json.loads(config.read_text())
            for entry in entries:
                entry['window_bytes'] = 4 * 1048576
            config.write_text(json.dumps(entries))
            argv = ['--trace', str(trace), '--devices-config', str(config),
                    '--output', str(root / 'out.json'), '--experiment', 'layout-concurrency', '--dry-run']
            with contextlib.redirect_stdout(io.StringIO()), patch('bmpclient.bench.layout_replay.DirectPool') as pool:
                result = run(parser().parse_args(argv))
                self.assertEqual(result['worker_sweep'], [1, 4, 8, 16])
                self.assertFalse(result['merge'])
                self.assertEqual(result['segments'], [1048576])
                for bad in (['--segments', '4K'], ['--segments', '64K', '1M'],
                            ['--worker-sweep', '0'], ['--mode', 'online']):
                    with self.assertRaises(ValueError):
                        run(parser().parse_args(argv + bad))
            pool.assert_not_called()

    def test_partial_extent_allocation_failure_reclaims_previous_devices(self):
        lib = FakeUMMLib(num_ssd_devices=8)
        allocate = lib.alloc_on_device
        def fail(size, tier, device):
            if device == 4:
                raise RuntimeError('injected device full')
            return allocate(size, tier, device)
        with patch.object(lib, 'alloc_on_device', side_effect=fail), self.assertRaisesRegex(RuntimeError, 'full'):
            VirtualMedia(lib, 4096, {d: 16384 for d in range(8)}, sp_bytes=16384, sp_bytes_per_device={})
        self.assertFalse(lib._lib._chunks)

    def prepare(self, root, kind='file'):
        trace = root / 'trace.jsonl'
        records = [dict(request_id='a', step_id=0, layer_id=19, context_length=65,
                        topk_token_indices=list(range(64)) + [5]),
                   dict(request_id='b', step_id=0, layer_id=31, context_length=17,
                        topk_token_indices=list(range(17))),
                   dict(request_id='a', step_id=1, layer_id=19, context_length=66,
                        topk_token_indices=[65, 1, 64, 5]),
                   dict(request_id='c', step_id=0, layer_id=2, context_length=1,
                        topk_token_indices=[0])]
        trace.write_text(''.join(json.dumps(r) + '\n' for r in records))
        manifest = root / 'devices.json'
        manifest.write_text(json.dumps([{kind: str(root / f'disk{d}'), 'window_offset': (d + 1) * 4096,
                                        'window_bytes': 1048576} for d in range(8)]))
        return trace, manifest

    def test_original_layer_hash_and_capacity_not_uniform_guess(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, _ = self.prepare(root)
            trace = load_trace(path, 2048)
            lengths, _ = geometry(trace, 16384, 'online')
            caps, rows = hash_capacities(trace, 16384, 'online', 8, lengths)
            expected = [0] * 8
            h = PositionHashStrategy(8, {})
            for token in range(66):
                expected[h.locate((19, token))] += 1
            self.assertEqual(rows['a'], expected)
            self.assertEqual(sum(rows['c']), 1)
            self.assertEqual(caps['c'], [16384] * 8)
            wrapper = OriginalLayerHash(8, [19, 31])
            self.assertEqual(wrapper.locate_batch([(0, 0), (1, 4)]),
                             [h.locate((19, 0)), h.locate((31, 4))])
            self.assertNotEqual(wrapper.locate((0, 0)), h.locate((0, 0)))

    def test_dry_run_rejects_bad_manifest_and_small_window_without_open(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace, config = self.prepare(root, 'device')
            args = parser().parse_args(['--trace', str(trace), '--devices-config', str(config),
                '--output', str(root / 'out.json'), '--segments', '16K', '--dry-run'])
            with patch('bmpclient.bench.layout_replay.DirectPool') as pool, contextlib.redirect_stdout(io.StringIO()):
                result = run(args)
                self.assertEqual(result['device_count'], 8)
                self.assertEqual(result['capacity_bytes'], sum(d['capacity_bytes'] for d in result['devices']))
                entries = json.loads(config.read_text())
                entries[7]['window_bytes'] = 4096
                config.write_text(json.dumps(entries))
                with self.assertRaisesRegex(ValueError, 'device 7'):
                    run(args)
                entries[7] = entries[0]
                config.write_text(json.dumps(entries))
                with self.assertRaisesRegex(ValueError, 'duplicate'):
                    load_devices(config)
                config.write_text(json.dumps(entries[:7]))
                with self.assertRaisesRegex(ValueError, 'eight'):
                    load_devices(config)
            pool.assert_not_called()
            self.assertFalse((root / 'out.json').exists())

    @unittest.skipUnless(os.environ.get('UMM_TEST_DIRECT') == '1', 'opt-in eight temporary file O_DIRECT integration')
    def test_eight_native_files_all_devices_stats_windows_and_release(self):
        for mode in ('snapshot', 'online'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix='umm-eight-') as directory:
                root = Path(directory)
                trace, config = self.prepare(root)
                args = parser().parse_args(['--trace', str(trace), '--devices-config', str(config),
                    '--output', str(root / 'out.json'), '--segments', '16K', '--repeats', '2', '--mode', mode])
                result = run(args)
                self.assertEqual(result['status'], 'complete')
                self.assertEqual(result['device_count'], 8)
                counts = []
                for trial in result['trials']:
                    stats = trial['io']
                    for key in ('read_calls', 'write_calls', 'read_bytes', 'write_bytes', 'bounce_bytes'):
                        self.assertEqual(stats[key], sum(d[key] for d in stats['devices']))
                    self.assertTrue(all(d['read_bytes'] > 0 and d['write_bytes'] > 0 for d in stats['devices']))
                    counts.append([d['write_bytes'] for d in stats['devices']])
                # Same write volume per device despite different insertion order.
                self.assertEqual(counts[0], counts[1])
                self.assertEqual(counts[2], counts[3])
                for device in result['devices']:
                    data = Path(device['target']).read_bytes()
                    start, end = device['possible_write_range']
                    self.assertEqual(data[:start], bytes(start))
                    self.assertEqual(data[end:], bytes(len(data) - end))


if __name__ == '__main__':
    unittest.main()
