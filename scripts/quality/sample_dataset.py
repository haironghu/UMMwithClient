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
    total = args.count if args.count is not None else len(rows)
    if total > len(rows):
        raise SystemExit(f"--count {total} exceeds dataset size {len(rows)}")
    # Order the WHOLE selection task-round-robin first, so consecutive chunks
    # (batches) each get a balanced task mix rather than one domain per batch.
    ordered = pick(rows, total, args.strategy, args.seed)
    if args.out_prefix:
        if args.batch_size < 1:
            raise SystemExit("--batch-size must be positive")
        batches = [ordered[i:i + args.batch_size] for i in range(0, len(ordered), args.batch_size)]
        summary = []
        for index, batch in enumerate(batches, 1):
            out = Path(f"{args.out_prefix}{index:02d}.jsonl")
            if out.exists():
                raise SystemExit(f"output exists: {out}")
            for row in batch:
                append_jsonl(out, row)
            counts = {}
            for row in batch:
                counts[row.get("task") or "default"] = counts.get(row.get("task") or "default", 0) + 1
            summary.append(dict(file=out.name, n=len(batch), by_task=dict(sorted(counts.items()))))
        print(json.dumps(dict(source=str(args.input), rows=len(ordered),
                              batch_size=args.batch_size, batches=summary), indent=2))
        return
    if not args.out:
        raise SystemExit("need --out or --out-prefix")
    out = Path(args.out)
    if out.exists():
        raise SystemExit(f"output exists: {out}")
    for row in ordered:
        append_jsonl(out, row)
    counts = {}
    for row in ordered:
        counts[row.get("task") or "default"] = counts.get(row.get("task") or "default", 0) + 1
    print(json.dumps(dict(source=str(args.input), requested=total, drawn=len(ordered),
                          by_task=dict(sorted(counts.items()))), indent=2))


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--input", required=True, help="normalized dataset JSONL")
    p.add_argument("--out", help="output subset JSONL (must be new)")
    p.add_argument("--out-prefix", help="split mode: write PREFIX01.jsonl, PREFIX02.jsonl, ...")
    p.add_argument("--batch-size", type=int, default=20, help="rows per split-mode batch file")
    p.add_argument("--count", type=int,
                   help="total rows to keep before splitting (default: all)")
    p.add_argument("--strategy", choices=("task-round-robin", "random"), default="task-round-robin")
    p.add_argument("--seed", type=int, default=2026)
    return p


if __name__ == "__main__":
    main(parser().parse_args())
