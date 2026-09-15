import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from bmpclient.bench.layout_replay import parser, run, geometry
from bmpclient.bench.multi_device import OriginalLayerHash, hash_capacities, load_devices
from bmpclient.bench.single_ssd import load_trace
from bmpclient.virtual_media_strategy import PositionHashStrategy
from bmpclient.testing import FakeUMMLib
from bmpclient.virtual_media import VirtualMedia


class TestMultiDevice(unittest.TestCase):
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
