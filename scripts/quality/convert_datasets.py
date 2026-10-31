#!/usr/bin/env python3
"""Normalize downloaded benchmarks into the runner's dataset JSONL.

Supported formats (download the raw files separately, e.g. via ModelScope or
HF mirror; see scripts/quality/README.md):
  ruler           jsonl rows {"id", "prompt", "ground_truth", ...} from NVIDIA/RULER
  longbench-v2    one JSON array with {"context","question","choice1..4","answer",...}
  infinitebench   one JSON array per task file with {"context","input","answer",...}

The prompts use plain wrappers. That is deliberate: the quality experiment
compares run A vs run B on identical prompts; scores here are NOT comparable
with public leaderboard numbers produced by official templates.
"""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import append_jsonl, http_json, auth_headers, sanitize_id  # noqa: E402


def rows_ruler(path):
    with open(path, encoding="utf-8") as stream:
        for index, line in enumerate(stream):
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            truth = row.get("ground_truth")
            yield dict(id=sanitize_id(row.get("id") or f"ruler_{index:04d}"),
                       task=f"ruler_{row.get('generator') or row.get('task') or 'gen'}",
                       prompt=row["prompt"], reference=truth,
                       gold_text=truth[0] if isinstance(truth, list) and truth else truth,
                       max_tokens=32, match="contain")


def rows_longbench_v2(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    letters = ["A", "B", "C", "D"]
    for index, row in enumerate(data):
        answer = str(row.get("answer") or "").strip().upper()[:1]
        options = [str(row.get(f"choice{i+1}") or "") for i in range(4)]
        prompt = (f"{row['context']}\n\nQuestion: {row['question']}\n"
                  + "\n".join(f"{l}. {o}" for l, o in zip(letters, options))
                  + "\nAnswer with the option's letter only.")
        yield dict(id=sanitize_id(f"lbv2_{index:04d}"),
                   task=f"lbv2_{row.get('domain') or 'any'}",
                   prompt=prompt, reference=[answer, f"({answer})"],
                   max_tokens=8, match="contain")


def rows_infinitebench(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    task = sanitize_id(Path(path).stem)
    for index, row in enumerate(data):
        answer = row.get("answer")
        prompt = f"{row['context']}\n\n{row['input']}"
        yield dict(id=sanitize_id(f"{task}_{index:04d}"), task=f"inf_{task}",
                   prompt=prompt, reference=answer,
                   gold_text=answer if isinstance(answer, str) and answer in row["context"] else None,
                   max_tokens=64, match="contain")


CONVERTERS = {"ruler": rows_ruler, "longbench-v2": rows_longbench_v2,
              "infinitebench": rows_infinitebench}


def main(args):
    out = Path(args.out)
    if out.exists():
        raise SystemExit(f"output exists: {out}")
    tokenizer_endpoint, headers = None, None
    if args.tokenize_endpoint:
        tokenizer_endpoint = args.tokenize_endpoint.rstrip("/")
        headers = auth_headers(args.api_key)
    written = kept = filtered = unscoreable = 0
    longest = 0
    for row in CONVERTERS[args.format](args.input):
        written += 1
        if row.get("reference") in (None, "", []):
            unscoreable += 1
            continue
        tokens = None
        if tokenizer_endpoint:
            try:
                payload = http_json(f"{tokenizer_endpoint}/tokenize",
                                    {"model": args.model, "prompt": row["prompt"]},
                                    args.timeout, headers)
                tokens = len(payload.get("tokens") or payload.get("token_ids") or [])
            except Exception as exc:  # endpoint unusable: warn once, stop using it
                print(f"tokenize endpoint unusable ({exc}); continuing without filtering", flush=True)
                tokenizer_endpoint = None
        row["prompt_chars"] = len(row["prompt"])
        if tokens is not None:
            row["approx_tokens"] = tokens
            longest = max(longest, tokens)
            if tokens + int(row.get("max_tokens") or 64) > args.max_prompt_tokens:
                filtered += 1
                continue
        kept += 1
        append_jsonl(out, row)
    stats = dict(format=args.format, input=str(args.input), rows=written, kept=kept,
                 filtered_by_tokens=filtered, unscoreable=unscoreable,
                 longest_prompt_tokens=longest or None)
    print(json.dumps(stats, indent=2))
    if kept == 0:
        raise SystemExit("no rows kept")


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--format", choices=sorted(CONVERTERS), required=True)
    p.add_argument("--input", required=True, help="raw dataset file (see module docstring)")
    p.add_argument("--out", required=True, help="output normalized JSONL (must be new)")
    p.add_argument("--tokenize-endpoint", help="vLLM base URL, e.g. http://host:8088")
    p.add_argument("--api-key")
    p.add_argument("--model", default="glm-5")
    p.add_argument("--max-prompt-tokens", type=int, default=190000)
    p.add_argument("--timeout", type=int, default=120)
    return p


if __name__ == "__main__":
    main(parser().parse_args())
