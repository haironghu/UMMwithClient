#!/usr/bin/env python3
"""Draw a small deterministic subset from a normalized dataset JSONL.

Smoke runs want every task represented rather than the first N rows in file
order: the default strategy round-robins over the `task` field (ids are
preserved, so A/B runs and dump subdirectories stay aligned with the full
dataset). `random` draws a seeded uniform sample instead.
"""
import argparse
import json
from pathlib import Path
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import append_jsonl, read_jsonl  # noqa: E402


def pick(rows, count, strategy, seed):
    if strategy == "random":
        return random.Random(seed).sample(rows, count)
    groups = {}
    for row in rows:
        groups.setdefault(row.get("task") or "default", []).append(row)
    picked = []
    while len(picked) < count:
        advanced = False
        for key in sorted(groups):
            if groups[key] and len(picked) < count:
                picked.append(groups[key].pop(0))
                advanced = True
        if not advanced:
            break
    return picked


def main(args):
    rows = list(read_jsonl(args.input))
    if args.count > len(rows):
        raise SystemExit(f"--count {args.count} exceeds dataset size {len(rows)}")
    picked = pick(rows, args.count, args.strategy, args.seed)
    out = Path(args.out)
    if out.exists():
        raise SystemExit(f"output exists: {out}")
    for row in picked:
        append_jsonl(out, row)
    counts = {}
    for row in picked:
        counts[row.get("task") or "default"] = counts.get(row.get("task") or "default", 0) + 1
    print(json.dumps(dict(source=str(args.input), requested=args.count, drawn=len(picked),
                          by_task=dict(sorted(counts.items()))), indent=2))


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--input", required=True, help="normalized dataset JSONL")
    p.add_argument("--out", required=True, help="output subset JSONL (must be new)")
    p.add_argument("--count", type=int, required=True)
    p.add_argument("--strategy", choices=("task-round-robin", "random"), default="task-round-robin")
    p.add_argument("--seed", type=int, default=2026)
    return p


if __name__ == "__main__":
    main(parser().parse_args())
