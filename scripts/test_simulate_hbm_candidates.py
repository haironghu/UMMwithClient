import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from simulate_hbm_candidates import BatchLRU, choose, measure


class SimulationTests(unittest.TestCase):
    def test_batch_hits_are_pinned_and_measured_before_admission(self):
        cache = BatchLRU(3)
        cache.commit({1, 2, 3})
        self.assertEqual(measure({1, 4, 5}, cache, set())["cache_hits"], 1)
        cache.commit({1, 4, 5})
        self.assertEqual(set(cache.rows), {1, 4, 5})

    def test_replacement_budget_and_unknown_native_rank(self):
        selected, count = choose({1, 2, 99}, [1, 2, 3, 4], {1, 3, 4}, 2)
        self.assertEqual(selected, {1, 3, 99})
        self.assertEqual(count, 1)
        selected, count = choose({1, 2}, [1, 2, 3], {3}, 0)
        self.assertEqual(selected, {1, 2})
        self.assertEqual(count, 0)

    def test_protection_and_current_token(self):
        selected, count = choose({1, 2}, [1, 2, 3], {3}, 2, protect_top=2)
        self.assertEqual((selected, count), ({1, 2}, 0))
        stats = measure({1, 2}, BatchLRU(2), {2})
        self.assertEqual(stats["fresh_hits"], 1)
        self.assertEqual(stats["misses"], 1)

    def test_replacement_saves_only_preexisting_hits(self):
        base, aware = BatchLRU(3), BatchLRU(3)
        base.commit({1, 4, 5})
        aware.commit({1, 4, 5})
        native = {1, 2, 3}
        chosen, n = choose(native, [1, 2, 3, 4, 5], aware.rows.keys(), 1)
        self.assertEqual(n, 1)
        self.assertEqual(measure(native, base, set())["misses"], 2)
        self.assertEqual(measure(chosen, aware, set())["misses"], 1)
        base.commit(native)
        aware.commit(chosen)
        self.assertNotEqual(set(base.rows), set(aware.rows))

    def test_end_to_end_warmup_and_zero_budget(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for seq in (3000, 3001, 3002):
                shift = seq - 3000
                data = dict(layer="layer.0", seq_lens_key=[seq], seq_lens_query=[1],
                            indices=[list(range(shift, shift + 2048))],
                            has_reference_candidates=True,
                            candidate_indices=list(range(shift, shift + 2400)),
                            candidate_scores=list(range(2400, 0, -1)))
                (root / f"layer_0_s{seq}.json").write_text(json.dumps(data))
            subprocess.run([sys.executable, str(Path(__file__).with_name("simulate_hbm_candidates.py")),
                            str(root), "--capacities", "2048", "--warmup-steps", "1",
                            "--max-replacements", "0", "--out-dir", str(root / "out")],
                           check=True, capture_output=True)
            report = json.loads((root / "out/summary.json").read_text())
            for row in report["results"]:
                self.assertEqual(row["selected"], 4096)
                self.assertEqual(row["misses"], 2)
                self.assertEqual(row["replacements"], 0)
                self.assertEqual(row["miss_reduction_vs_baseline"], 0)


if __name__ == "__main__":
    unittest.main()
