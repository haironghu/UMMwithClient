import json
import os
from pathlib import Path
import tempfile
import unittest

from bmpclient.bench.layout_replay import parser, run, summarize_read_batches, validate_miss_trace
from bmpclient.bench.single_ssd import load_trace


class MissReplayTests(unittest.TestCase):
    def make_trace(self, root, measured_ids):
        records = []
        for step, ids in enumerate(([0, 1], measured_ids, [])):
            records.append(dict(request_id='sample', step_id=step, layer_id=27,
                                context_length=8 + step, topk_token_indices=ids,
                                warmup=step == 0, selected=3, cache_hits=3 - len(ids),
                                fresh_hits=0, misses=len(ids), replacements=0,
                                cache_capacity=4096, cache_strategy='baseline'))
        path = root / 'trace.jsonl'
        path.write_text(''.join(json.dumps(r) + '\n' for r in records))
        return path

    def test_empty_only_opt_in_and_count_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.make_trace(Path(tmp), [2])
            with self.assertRaises(ValueError):
                load_trace(path, 2048)
            trace = load_trace(path, 2048, allow_empty=True)
            self.assertEqual(validate_miss_trace(trace)['capacity_rows_per_layer'], 4096)
            trace.records[1]['misses'] += 1
            with self.assertRaises(ValueError):
                validate_miss_trace(trace)

    def test_summary_excludes_warmup_keeps_zero_batches(self):
        records = [dict(request_id='s', step_id=0, unique_topk=10, fetch_us=1000,
                        plan_us=10, warmup=True, io={'read_bytes': 40960}),
                   dict(request_id='s', step_id=1, unique_topk=2, fetch_us=20,
                        plan_us=2, warmup=False, io={'read_bytes': 8192}),
                   dict(request_id='s', step_id=1, unique_topk=0, fetch_us=0,
                        plan_us=0, warmup=False, io={'read_bytes': 0})]
        summary = summarize_read_batches(records)
        self.assertEqual(summary['measured_read_bytes'], 8192)
        self.assertEqual(summary['measured_miss_rows'], 2)
        self.assertEqual(summary['zero_miss_batches'], 1)
        self.assertEqual(summary['decode_step_read_us']['p95'], 20)
        self.assertEqual(summary['active_fetch_seconds'], 0.00002)

    @unittest.skipUnless(os.environ.get('UMM_TEST_DIRECT') == '1', 'temporary-file O_DIRECT integration')
    def test_eight_file_hash_read_bytes(self):
        with tempfile.TemporaryDirectory(prefix='umm-hbm-eight-') as tmp:
            root = Path(tmp)
            trace = self.make_trace(root, [2, 3])
            config = root / 'devices.json'
            config.write_text(json.dumps([dict(file=str(root / f'disk{i}.raw'),
                                                window_offset=4096, window_bytes=65536)
                                          for i in range(8)]))
            args = parser().parse_args(['--trace', str(trace), '--devices-config', str(config),
                '--output', str(root / 'out.json'), '--miss-trace', '--experiment', 'layout-concurrency',
                '--layouts', 'ordered', '--segments', '16K', '--worker-sweep', '16', '--repeats', '1'])
            result = run(args)
            trial = result['trials'][0]
            self.assertEqual(result['device_count'], 8)
            self.assertEqual(trial['measured_read_bytes'], 8192)
            self.assertEqual(sum(d['read_bytes'] for d in trial['batches'][1]['io']['devices']), 8192)
            self.assertEqual(trial['batches'][2]['io']['read_calls'], 0)

    @unittest.skipUnless(os.environ.get('UMM_TEST_DIRECT') == '1', 'temporary-file O_DIRECT integration')
    def test_native_miss_only_and_zero_miss(self):
        for ids in ([2, 3], []):
            with self.subTest(ids=ids), tempfile.TemporaryDirectory(prefix='umm-hbm-miss-') as tmp:
                root = Path(tmp)
                trace = self.make_trace(root, ids)
                args = parser().parse_args(['--trace', str(trace), '--file', str(root / 'data.raw'),
                    '--output', str(root / 'out.json'), '--miss-trace', '--experiment', 'layout-concurrency',
                    '--layouts', 'ordered', '--segments', '16K', '--worker-sweep', '2', '--repeats', '1'])
                result = run(args)
                trial = result['trials'][0]
                self.assertEqual(result['status'], 'complete')
                self.assertEqual(trial['written_rows'], 10)
                self.assertEqual(trial['measured_read_bytes'], len(ids) * 4096)
                self.assertEqual(trial['io']['read_bytes'], (2 + len(ids)) * 4096)
                self.assertEqual(trial['batches'][-1]['fetch_us'], 0)
                self.assertEqual(trial['batches'][-1]['io']['read_calls'], 0)
                self.assertEqual(trial['measured_miss_rows'], len(ids))


if __name__ == '__main__':
    unittest.main()
