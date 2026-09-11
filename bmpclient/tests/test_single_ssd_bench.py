import json
from pathlib import Path
import tempfile
import unittest
from bmpclient.bench.single_ssd import load_trace, plan_rows, pattern, percentiles, parser, run


class TestSingleSSDTrace(unittest.TestCase):
    def load(self, records):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text("\n".join(json.dumps(r) for r in records))
            return load_trace(path, 2048)

    def record(self, **kwargs):
        r = dict(request_id="r", step_id=0, layer_id=0, context_length=8,
                 topk_token_indices=[0, 1, 7])
        r.update(kwargs)
        return r

    def test_snapshot_bounds_and_order(self):
        trace = self.load([self.record(layer_id=1), self.record(),
                           self.record(step_id=1, context_length=10, topk_token_indices=[9])])
        self.assertEqual(trace.rows, 18)
        self.assertEqual(trace.bases, {("r", 0): 0, ("r", 1): 10})
        self.assertEqual([r["layer_id"] for r in trace.records], [1, 0, 0])

    def test_invalid_trace_rejected(self):
        for delta in [dict(topk_token_indices=[-1]), dict(topk_token_indices=[8]),
                      dict(topk_token_indices=[True]), dict(topk_token_indices=[]),
                      dict(topk_token_indices=[0] * 2049), dict(context_length=0),
                      dict(request_id=1), dict(step_id=-1)]:
            with self.subTest(delta=delta), self.assertRaises(ValueError):
                self.load([self.record(**delta)])
        with self.assertRaises(ValueError):
            self.load([])

    def test_merge_is_bounded_no_overread_duplicates_preserve_order(self):
        rows, slots, runs = plan_rows([7, 2, 1, 2, 0, 3], 10, True, 8192)
        self.assertEqual(rows, [10, 11, 12, 13, 17])
        self.assertEqual([rows[s] for s in slots], [17, 12, 11, 12, 10, 13])
        self.assertEqual(runs, [(10, 0, 2), (12, 2, 2), (17, 4, 1)])
        self.assertEqual(sum(r[2] for r in runs), len(rows))
        self.assertEqual(len(plan_rows([0, 1, 2], 0, False, 8192)[2]), 3)

    def test_pattern_and_percentiles(self):
        self.assertEqual(len(pattern(1, 2)), 4096)
        self.assertEqual(pattern(1, 2), pattern(1, 2))
        self.assertNotEqual(pattern(1, 2), pattern(2, 2))
        self.assertEqual(percentiles([100, 1, 10]), {"p50": 10, "p95": 100, "p99": 100})

    def test_refusals_preserve_existing_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace, target, output = (root / name for name in ("trace.jsonl", "data", "out.json"))
            trace.write_text(json.dumps(self.record()))
            target.write_bytes(b"keep existing data")
            argv = ["--trace", str(trace), "--file", str(target), "--output", str(output)]
            with self.assertRaises(FileExistsError):
                run(parser().parse_args(argv))
            with self.assertRaises(ValueError):
                run(parser().parse_args(argv + ["--reuse-file"]))
            self.assertEqual(target.read_bytes(), b"keep existing data")
            self.assertFalse(output.exists())

    def test_trace_alias_and_invalid_window_rejected_before_write(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace, target, output = (root / name for name in ("trace.jsonl", "data", "out.json"))
            original = json.dumps(self.record())
            trace.write_text(original)
            argv = ["--trace", str(trace), "--file", str(target), "--output", str(output)]
            with self.assertRaises(ValueError):
                run(parser().parse_args(argv + ["--window-offset", "1"]))
            self.assertFalse(target.exists())
            import os
            os.link(trace, target)
            with self.assertRaises(ValueError):
                run(parser().parse_args(argv + ["--reuse-file"]))
            self.assertEqual(trace.read_text(), original)
            self.assertFalse(output.exists())
