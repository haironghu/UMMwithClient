import json
import os
from pathlib import Path
import tempfile
import unittest

from bmpclient.bench.convert_trace import convert
from bmpclient.bench.layout_replay import geometry, parser, run
from bmpclient.bench.single_ssd import load_trace
from bmpclient.testing import FakeUMMLib
from bmpclient.virtual_media import VirtualMedia


class TestLayoutReplay(unittest.TestCase):
    def records(self):
        return [dict(request_id='a', step_id=0, layer_id=3, context_length=5, topk_token_indices=[4, 0, 4]),
                dict(request_id='b', step_id=0, layer_id=0, context_length=8, topk_token_indices=[7, 1, 0]),
                dict(request_id='a', step_id=1, layer_id=3, context_length=6, topk_token_indices=[5, 0]),
                dict(request_id='b', step_id=1, layer_id=0, context_length=9, topk_token_indices=[8, 7]),
                dict(request_id='c', step_id=0, layer_id=2, context_length=3, topk_token_indices=[2, 0])]

    def write_trace(self, root):
        path = root / 'trace.jsonl'
        path.write_text(''.join(json.dumps(r) + '\n' for r in self.records()))
        return path

    def test_geometry_padding_and_causality(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_trace(Path(directory))
            trace = load_trace(path, 2048)
            lengths, caps = geometry(trace, 16384, 'online')
            self.assertEqual(caps, {'a': 49152, 'b': 49152, 'c': 16384})
            self.assertEqual(geometry(trace, 16384, 'snapshot')[1], {'a': 32768, 'b': 49152, 'c': 16384})
            trace.records[2]['context_length'] = 4
            with self.assertRaises(ValueError):
                geometry(trace, 16384, 'online')
            trace.records[2]['context_length'] = 6
            trace.records[0]['step_id'] = 2
            with self.assertRaises(ValueError):
                geometry(trace, 16384, 'online')

    def test_read_policy_preserves_buffers_and_bounds(self):
        for merge in (False, True):
            lib = FakeUMMLib(num_ssd_devices=1)
            with VirtualMedia(lib, 4096, 65536, sp_bytes=16384, sp_bytes_per_device={},
                              read_merge=merge, max_read_bytes=8192) as vm:
                offsets = [vm.write((0, t), bytes([t]) * 4096) for t in range(8)]
                vm.flush()
                tokens = [7, 2, 1, 2, 0, 3]
                out = [memoryview(bytearray(4096)) for _ in tokens]
                vm.read_batch([(0, offsets[t]) for t in tokens], out)
                self.assertEqual([bytes(b) for b in out], [bytes([t]) * 4096 for t in tokens])
                runs = vm._merge_runs([(i, i * 4096) for i in range(8)])
                self.assertTrue(all(len(r) <= (2 if merge else 1) for r in runs))
        with self.assertRaises(ValueError):
            VirtualMedia(FakeUMMLib(num_ssd_devices=1), 4096, 65536, max_read_bytes=1)

    def test_converter_mapping_and_missing_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / 'in.json', root / 'out.jsonl'
            source.write_text(json.dumps([{'ids': [3, 0, 3], 'rid': 'r'}]))
            result = convert(source, output, {'topk_token_indices': 'ids', 'request_id': 'rid'},
                             {'layer_id': 7, 'context_length': 4}, input_format='json')
            self.assertEqual(result['records'], 1)
            self.assertEqual(load_trace(output, 2048).records[0]['topk_token_indices'], [3, 0, 3])
            with self.assertRaises(FileExistsError):
                convert(source, output)
            source.write_text('[1, 2]\n[2, 3]\n')
            invalid = root / 'invalid.jsonl'
            with self.assertRaises(ValueError):
                convert(source, invalid)
            self.assertFalse(invalid.exists())
            result = convert(source, invalid, defaults={'request_id': 'r', 'layer_id': 0, 'context_length': 4})
            self.assertEqual(result['records'], 2)
            self.assertEqual([r['step_id'] for r in load_trace(invalid, 2048).records], [0, 1])

    def test_invalid_run_does_not_create_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.write_trace(root)
            output, target = root / 'out.json', root / 'data.raw'
            args = parser().parse_args(['--trace', str(trace), '--file', str(target), '--output', str(output),
                                        '--segments', '26640384'])
            with self.assertRaises(ValueError):
                run(args)
            self.assertFalse(target.exists())
            self.assertFalse(output.exists())

    @unittest.skipUnless(os.environ.get('UMM_TEST_DIRECT') == '1', 'opt-in native O_DIRECT temporary-file integration')
    def test_native_snapshot_online_layouts_and_reclamation(self):
        with tempfile.TemporaryDirectory(prefix='umm-layout-test-') as directory:
            root = Path(directory)
            trace = self.write_trace(root)
            for mode in ('snapshot', 'online'):
                args = parser().parse_args(['--trace', str(trace), '--file', str(root / (mode + '.raw')),
                    '--output', str(root / (mode + '.json')), '--segments', '16K', '--repeats', '2', '--mode', mode])
                result = run(args)
                self.assertEqual(result['status'], 'complete')
                self.assertEqual(len(result['trials']), 8)
                self.assertEqual(len(result['comparisons']), 5)
                hashes = {}
                for t in result['trials']:
                    self.assertEqual(t['written_rows'], 18)
                    self.assertEqual(len(t['batches']), 5)
                    self.assertGreaterEqual(t['write_amplification_vs_slots'], 1)
                    self.assertGreater(t['io']['read_calls'], 0)
                    key = (t['segment_bytes'], t['layout'])
                    if key in hashes:
                        self.assertEqual(t['mapping_sha256'], hashes[key])
                    hashes[key] = t['mapping_sha256']
                self.assertNotEqual(hashes[(4096, 'ordered')], hashes[(4096, 'shuffled')])
                small, large = result['trials'][0], result['trials'][2]
                self.assertLess(large['io']['write_calls'], small['io']['write_calls'])
                before = (root / (mode + '.raw')).read_bytes()
                with self.assertRaises(FileExistsError):
                    run(args)
                self.assertEqual((root / (mode + '.raw')).read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
