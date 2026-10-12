import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from export_hbm_miss_traces import export, parser


class ExportTests(unittest.TestCase):
    def test_matches_simulator_and_orders_numeric_layers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sample = root / 'sample'
            sample.mkdir()
            for layer in (10, 2):
                for step, seq in enumerate((3000, 3001, 3002)):
                    start = step * 100
                    record = dict(layer=f'model.layers.{layer}.self_attn.attn',
                                  indices=[list(range(start, start + 2048))],
                                  seq_lens_key=[seq], seq_lens_query=[1],
                                  has_reference_candidates=True,
                                  candidate_indices=list(range(start, start + 2400)),
                                  candidate_scores=list(range(2400, 0, -1)))
                    (sample / f'model_layers_{layer}_s{seq}.json').write_text(json.dumps(record))
            out = root / 'export'
            manifest = export(parser().parse_args([str(sample), '--out-dir', str(out),
                                                  '--warmup-steps', '1', '--expected-layers', '2']))
            sim = root / 'sim'
            subprocess.run([sys.executable, str(Path(__file__).with_name('simulate_hbm_candidates.py')),
                            str(sample), '--capacities', '4096', '8192', '--warmup-steps', '1',
                            '--out-dir', str(sim)], check=True, capture_output=True)
            summaries = json.loads((sim / 'summary.json').read_text())['results']
            for item in manifest['files']:
                rows = [json.loads(line) for line in (out / item['file']).read_text().splitlines()]
                self.assertEqual([(r['step_id'], r['layer_id']) for r in rows],
                                 [(0, 2), (0, 10), (1, 2), (1, 10), (2, 2), (2, 10)])
                expected = next(r for r in summaries if r['capacity_rows_per_layer'] == item['capacity']
                                and r['strategy'] == item['strategy'])
                self.assertEqual(item['measured_totals']['misses'], expected['misses'])
                self.assertEqual(sum(len(r['topk_token_indices']) for r in rows if not r['warmup']), expected['misses'])
                self.assertEqual(item['measured_totals']['replacements'], expected['replacements'])
            self.assertFalse((out / 'spool.sqlite').exists())
            # The six-case driver must be safe by default: plans only, no files/devices opened.
            devices = root / 'devices.json'
            disk_paths = [root / f'disk{i}.raw' for i in range(8)]
            devices.write_text(json.dumps([dict(file=str(path), window_offset=0,
                                                 window_bytes=67108864) for path in disk_paths]))
            plan_dir = root / 'plan'
            subprocess.run([sys.executable, str(Path(__file__).with_name('bench_hbm_misses.py')),
                            '--traces-dir', str(out), '--devices-config', str(devices),
                            '--out-dir', str(plan_dir), '--workers', '16'],
                           check=True, capture_output=True)
            self.assertEqual(len(list(plan_dir.glob('*.log'))), 6)
            self.assertFalse(any(path.exists() for path in disk_paths))

    def test_retains_zero_miss_steps(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sample = root / 'sample'
            sample.mkdir()
            for seq in (10, 11):
                record = dict(layer='model.layers.0.self_attn.attn', indices=list(range(seq)),
                              seq_lens_key=[seq], seq_lens_query=[1], has_reference_candidates=True,
                              candidate_indices=list(range(seq)), candidate_scores=list(range(seq, 0, -1)))
                (sample / f'model_layers_0_s{seq}.json').write_text(json.dumps(record))
            out = root / 'out'
            export(parser().parse_args([str(sample), '--out-dir', str(out), '--warmup-steps', '1']))
            rows = [json.loads(line) for line in (out / 'c4096-baseline.jsonl').read_text().splitlines()]
            self.assertEqual(rows[-1]['topk_token_indices'], [])
            self.assertEqual(rows[-1]['context_length'], 11)
            self.assertFalse(rows[-1]['warmup'])


if __name__ == '__main__':
    unittest.main()
