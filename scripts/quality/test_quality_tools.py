"""Offline tests for the quality-collection tools (fake HTTP server, no torch)."""
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import analyze_quality as aq  # noqa: E402
import convert_datasets as cd  # noqa: E402
import make_passkey_dataset as pk  # noqa: E402
import reconcile_swaps as rs  # noqa: E402
import run_quality_collection as rqc  # noqa: E402
import sample_dataset as sd  # noqa: E402
from common import extract_option, match_answer, sanitize_id  # noqa: E402


class CommonTests(unittest.TestCase):
    def test_sanitize_matches_hook_behaviour(self):
        self.assertEqual(sanitize_id("model.layers.5.self_attn"), "model.layers.5.self_attn")
        self.assertEqual(sanitize_id("中文/样本:002"), "_002")
        self.assertEqual(sanitize_id("  "), "sample")

    def test_match_answer_modes(self):
        self.assertTrue(match_answer("The key is 40361.", ["40361"]))
        self.assertFalse(match_answer("40361", ["40362"]))
        self.assertTrue(match_answer("(A)", ["A", "(A)"], "contain"))
        self.assertFalse(match_answer("B", ["A"], "exact"))
        self.assertIsNone(match_answer("x", []))

    def test_option_extraction(self):
        self.assertEqual(extract_option("C"), "C")
        self.assertEqual(extract_option("B."), "B")
        self.assertEqual(extract_option("(D)"), "D")
        self.assertEqual(extract_option("A. Because the table says so"), "A")
        self.assertEqual(extract_option("Answer: C."), "C")
        self.assertEqual(extract_option("The answer is (D)."), "D")
        self.assertEqual(extract_option("我认为选 B"), "B")
        self.assertEqual(extract_option("答案：C"), "C")
        self.assertEqual(extract_option("The correct option is C"), "C")
        # Mid-sentence letters and English articles must not masquerade as options.
        self.assertIsNone(extract_option("I believe it is not a"))
        self.assertIsNone(extract_option("a bit of everything"))
        self.assertEqual(extract_option("C is wrong, so the answer is B"), "B")
        self.assertTrue(match_answer("I believe it is not a", ["A"], "option") is False)
        self.assertTrue(match_answer("Answer: A", ["A"], "option"))
        self.assertFalse(match_answer("C is wrong, so the answer is B", ["A"], "option"))
        self.assertFalse(match_answer("no letter at all", ["C"], "option"))


class PasskeyTests(unittest.TestCase):
    def test_sizing_needle_and_uniqueness(self):
        import random
        from common import append_jsonl
        rng = random.Random(2026)
        measure = pk.token_counter(None, chars_per_token=4.0)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "ds.jsonl"
            for depth in (0.05, 0.55, 0.95):
                prompt, keys = pk.build_prompt(rng, measure, 1000, depth, 5, 1)
                append_jsonl(out, dict(id=sanitize_id(f"pk_{depth}"), task="passkey",
                                       prompt=prompt, reference=keys[0], gold_text=keys[0],
                                       max_tokens=32, match="contain"))
            rows = [json.loads(line) for line in out.read_text().splitlines()]
            self.assertEqual(len({r["reference"] for r in rows}), 3)
            for row, depth in zip(rows, (0.05, 0.55, 0.95)):
                prompt = row["prompt"]
                self.assertEqual(prompt.count(row["reference"]), 1)
                total = measure(prompt)
                self.assertGreaterEqual(total, 950)
                self.assertLessEqual(total, 1060)
                position = prompt.index(row["reference"]) / len(prompt)
                self.assertLess(abs(position - depth), 0.15)

    def test_deterministic_with_same_seed(self):
        rng_a, rng_b = __import__("random").Random(7), __import__("random").Random(7)
        measure = pk.token_counter(None, 4.0)
        a = pk.build_prompt(rng_a, measure, 800, 0.5, 5, 1)
        b = pk.build_prompt(rng_b, measure, 800, 0.5, 5, 1)
        self.assertEqual(a[0], b[0])


class ConverterTests(unittest.TestCase):
    def test_longbench_v2(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp) / "lbv2.json"
            raw.write_text(json.dumps([dict(context="ctx " * 50, question="q?",
                                            choice1="one", choice2="two", choice3="tri",
                                            choice4="fyra", answer="C", domain="metric")]))
            out = Path(tmp) / "n.jsonl"
            args = cd.parser().parse_args(["--format", "longbench-v2", "--input", str(raw),
                                           "--out", str(out)])
            cd.main(args)
            rows = [json.loads(line) for line in out.read_text().splitlines()]
            self.assertEqual(len(rows), 1)
            self.assertIn("C.", rows[0]["prompt"])
            self.assertEqual(rows[0]["reference"], ["C", "(C)"])

    def test_infinitebench_gold_only_when_verbatim(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp) / "longbook_qa_eng.json"
            raw.write_text(json.dumps([
                dict(context="she keeps the ledger in the blue drawer", input="Where is the ledger?",
                     answer="blue drawer"),
                dict(context="the answer is implied only", input="?", answer="not present"),
            ]))
            out = Path(tmp) / "n.jsonl"
            cd.main(cd.parser().parse_args(["--format", "infinitebench", "--input", str(raw),
                                            "--out", str(out)]))
            rows = [json.loads(line) for line in out.read_text().splitlines()]
            self.assertEqual(rows[0]["gold_text"], "blue drawer")
            self.assertIsNone(rows[1]["gold_text"])

    def test_ruler(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp) / "ruler.jsonl"
            raw.write_text(json.dumps(dict(id="niah_1", prompt="p", ground_truth=["42"],
                                           generator="niah_single_1")) + "\n"
                           + json.dumps(dict(prompt="q", ground_truth="x")) + "\n")
            out = Path(tmp) / "n.jsonl"
            cd.main(cd.parser().parse_args(["--format", "ruler", "--input", str(raw),
                                            "--out", str(out)]))
            rows = [json.loads(line) for line in out.read_text().splitlines()]
            self.assertEqual(rows[0]["id"], "niah_1")
            self.assertEqual(rows[1]["reference"], "x")


class SamplerTests(unittest.TestCase):
    def rows(self):
        rows = []
        for task, n in (("single", 10), ("multi", 5), ("code", 2)):
            rows += [dict(id=f"{task}_{i:02d}", task=task, prompt=f"p{i}") for i in range(n)]
        return rows

    def test_round_robin_covers_every_task(self):
        picked = sd.pick(self.rows(), 6, "task-round-robin", 2026)
        tasks = [r["task"] for r in picked]
        self.assertEqual(len(picked), 6)
        self.assertEqual(set(tasks), {"single", "multi", "code"})
        self.assertEqual(tasks, ["code", "multi", "single", "code", "multi", "single"])

    def test_random_is_deterministic_and_subset(self):
        rows = self.rows()
        first = sd.pick(rows, 5, "random", 7)
        second = sd.pick(rows, 5, "random", 7)
        self.assertEqual([r["id"] for r in first], [r["id"] for r in second])
        self.assertTrue({r["id"] for r in first} <= {r["id"] for r in rows})

    def test_count_over_dataset_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "in.jsonl"
            for row in self.rows():
                from common import append_jsonl
                append_jsonl(src, row)
            with self.assertRaises(SystemExit):
                sd.main(sd.parser().parse_args(["--input", str(src), "--out",
                                                str(Path(tmp) / "out.jsonl"), "--count", "99"]))


class FakeServer:
    """Minimal vLLM-compatible endpoint recording the marker file on each call."""

    def __init__(self, marker_path, fail_first=0):
        self.marker_path = marker_path
        self.seen_markers = []
        self.requests = 0
        self.fail_first = fail_first
        handler = self._handler()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def _handler(self):
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def _send(self, payload, code=200):
                body = json.dumps(payload).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                payload = json.loads(self.rfile.read(length) or b"{}")
                if self.path.endswith("/tokenize"):
                    self._send({"tokens": [0] * (len(payload.get("prompt", "")) // 4)})
                    return
                if self.path.endswith("/completions"):
                    outer.requests += 1
                    if outer.marker_path.exists():
                        outer.seen_markers.append(outer.marker_path.read_text().strip())
                    if outer.requests <= outer.fail_first:
                        self._send({"error": "boom"}, 500)
                        return
                    key = payload["prompt"].split()[-1] if payload["prompt"].split() else "?"
                    tokens = ["the", "answer", "is", key]
                    self._send({"choices": [dict(text=" ".join(tokens), finish_reason="stop",
                                                 logprobs=dict(tokens=tokens,
                                                               token_logprobs=[-0.1, -0.2, -0.3, -0.4]))],
                                "usage": dict(prompt_tokens=len(payload["prompt"]) // 4,
                                              completion_tokens=len(tokens))})

            def log_message(self, *args):
                pass

        return Handler

    @property
    def endpoint(self):
        return f"http://127.0.0.1:{self.server.server_address[1]}/v1"

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


def make_dataset(path, rows):
    from common import append_jsonl
    for row in rows:
        append_jsonl(path, row)


class RunnerTests(unittest.TestCase):
    def dataset(self, tmp, rows):
        path = Path(tmp) / "ds.jsonl"
        make_dataset(path, rows)
        return path

    def run_main(self, dataset, out_dir, server, extra=()):
        argv = ["--dataset", str(dataset), "--out-dir", str(out_dir),
                "--endpoint", server.endpoint, "--model", "m",
                "--max-prompt-tokens", "1000"] + list(extra)
        return rqc.main(rqc.parser().parse_args(argv))

    def test_end_to_end_markers_filter_and_resume(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dump_root = root / "dumps"
            dump_root.mkdir()
            dataset = self.dataset(root, [
                dict(id="short_1", task="t", prompt="x " * 40, reference="is", max_tokens=8),
                dict(id="short_2", task="t", prompt="y " * 40, reference="is", max_tokens=8),
                dict(id="too_long", task="t", prompt="z " * 2000, reference="is", max_tokens=8),
            ])
            server = FakeServer(dump_root / "current_sample")
            try:
                out = root / "run"
                self.assertEqual(self.run_main(dataset, out, server,
                                               ("--dump-root", str(dump_root))), 0)
                rows = [json.loads(line) for line in (out / "results.jsonl").read_text().splitlines()]
                by_id = {r["id"]: r for r in rows}
                self.assertTrue(by_id["short_1"]["ok"])
                self.assertEqual(by_id["short_1"]["tokens"], ["the", "answer", "is", "x"])
                self.assertTrue(by_id["too_long"]["filtered"])
                self.assertEqual(server.seen_markers, ["short_1", "short_2"])
                self.assertFalse((dump_root / "current_sample").exists())
                manifest = json.loads((out / "manifest.json").read_text())
                self.assertEqual(manifest["status"], "complete")
                self.assertEqual(manifest["completed"], 2)
                self.assertEqual(manifest["filtered"], 1)
                # Rerunning with the same label resumes: no new completions.
                before = server.requests
                self.assertEqual(self.run_main(dataset, out, server,
                                               ("--dump-root", str(dump_root))), 0)
                self.assertEqual(server.requests, before)
                # A different label refuses without --append-ok.
                with self.assertRaises(SystemExit):
                    self.run_main(dataset, root / "run", server,
                                  ("--label", "B", "--dump-root", str(dump_root)))
            finally:
                server.stop()

    def test_retry_and_failure_recording(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = self.dataset(root, [dict(id="a", task="t", prompt="q " * 10,
                                               reference="is", max_tokens=8)])
            server = FakeServer(root / "marker", fail_first=1)
            try:
                out = root / "run"
                self.assertEqual(self.run_main(dataset, out, server), 0)
                rows = [json.loads(line) for line in (out / "results.jsonl").read_text().splitlines()]
                self.assertEqual(rows[0]["attempts"], 2)
                self.assertTrue(rows[0]["ok"])
            finally:
                server.stop()


class AnalyzerTests(unittest.TestCase):
    def test_metrics(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / "ds.jsonl"
            make_dataset(dataset, [
                dict(id="agree", task="t", prompt="p", reference="42", match="contain"),
                dict(id="regress", task="t", prompt="p", reference="42", match="contain"),
                dict(id="diverge", task="u", prompt="p", reference="42", match="contain"),
            ])

            def write(name, rows):
                path = root / name
                for row in rows:
                    from common import append_jsonl
                    append_jsonl(path, row)

            write("a.jsonl", [
                dict(id="agree", ok=True, task="t", output="it is 42",
                     tokens=["it", "is", "42"], token_logprobs=[-0.1, -0.1, -0.1]),
                dict(id="regress", ok=True, task="t", output="it is 42",
                     tokens=["it", "is", "42"], token_logprobs=[-0.1, -0.2, -0.3]),
                dict(id="diverge", ok=True, task="u", output="one two",
                     tokens=["one", "two"], token_logprobs=[-0.5, -0.5]),
            ])
            write("b.jsonl", [
                dict(id="agree", ok=True, task="t", output="it is 42",
                     tokens=["it", "is", "42"], token_logprobs=[-0.2, -0.1, -0.2]),
                dict(id="regress", ok=True, task="t", output="it is 99",
                     tokens=["it", "is", "99"], token_logprobs=[-0.1, -0.2, -0.4]),
                dict(id="diverge", ok=True, task="u", output="one three",
                     tokens=["one", "three"], token_logprobs=[-0.7, -0.9]),
            ])
            aq.main(aq.parser().parse_args(["--dataset", str(dataset), "--results-a", str(root / "a.jsonl"),
                                            "--results-b", str(root / "b.jsonl"),
                                            "--out-dir", str(root / "out")]))
            summary = json.loads((root / "out/summary.json").read_text())
            self.assertEqual(summary["overall"]["n"], 3)
            self.assertAlmostEqual(summary["overall"]["score_a"], round(2 / 3, 4))
            self.assertAlmostEqual(summary["overall"]["score_b"], round(1 / 3, 4))
            self.assertEqual(summary["regressions"], ["regress"])
            # teacher-forced deltas exist only for agreeing pairs with aligned logprobs
            self.assertAlmostEqual(summary["overall"]["lp_delta_mean"], (-0.0666666), places=5)
            self.assertEqual(summary["by_task"]["t"]["agreement"], 0.5)


class WordTokenizer:
    def __call__(self, text, add_special_tokens=False, return_offsets_mapping=False):
        ids, offsets, index = [], [], 0
        for word in text.split():
            start = text.index(word, index)
            ids.append(abs(hash(word)) % 100000)
            offsets.append((start, start + len(word)))
            index = start + len(word)
        return {"input_ids": ids, "offset_mapping": offsets}


class ReconcileTests(unittest.TestCase):
    def test_gold_swap_accounting(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            prompt = "alpha secret40361 beta filler tail"
            gold_id = WordTokenizer()(prompt)["input_ids"][1]
            dump = root / "dumps" / "example_1"
            dump.mkdir(parents=True)
            for seq, native_has_gold in ((100, True), (101, False)):
                native = [0, 1, 2, gold_id] if native_has_gold else [0, 1, 2, 7]
                selected = [0, 1, 2, 7] if native_has_gold else native
                swaps = [[gold_id, 7]] if native_has_gold else []
                (dump / f"model.layers.0.self_attn_s{seq}.json").write_text(json.dumps(dict(
                    layer="model.layers.0.self_attn", seq_lens_key=[seq], seq_lens_query=[1],
                    indices=native, selected=selected, swaps=swaps, overridden=True,
                    cache_mode="shared", cache_state_sha256="x", layer_rank=0, stride_tokens=100)))
            example = dict(id="example_1", task="t", prompt=prompt, reference="secret40361",
                           gold_text="secret40361", max_tokens=8, match="contain")
            stats, error = rs.reconcile_example(root / "dumps", example, WordTokenizer())
            self.assertIsNone(error)
            self.assertEqual(stats["gold_tokens"], 1)
            self.assertEqual(stats["records"], 2)
            self.assertEqual(stats["gold_swapped_out"], 1)
            self.assertEqual(stats["min_gold_in_selected"], 0)
            self.assertTrue(stats["gold_ever_absent"])

    def test_missing_gold_and_missing_dump(self):
        example = dict(id="gone", task="t", prompt="nothing here", reference="absent",
                       gold_text="absent", match="contain")
        stats, error = rs.reconcile_example(Path("nowhere"), example, WordTokenizer())
        self.assertIsNone(stats)
        self.assertIn("no dump directory", error)
        example2 = dict(id="gone", task="t", prompt="no literal answer", reference="zzz",
                        match="contain")
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "gone").mkdir()
            stats, error = rs.reconcile_example(tmp, example2, WordTokenizer())
            self.assertIsNone(stats)
            self.assertIn("not found", error)


if __name__ == "__main__":
    unittest.main()
