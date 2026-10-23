#!/usr/bin/env python3
"""Replay one single-request DSA dump directory; no GPU required.

Per-layer token-row caches, initially empty, with batch LRU. Each batch is
one recency timestamp; ties are resolved by increasing token ID (not score).
The just-generated KV can optionally be available outside the persistent
cache. Only selected rows are admitted. No prefetch, shared prefix, or I/O
page amplification is modeled. This is fixed-baseline-trajectory replay,
not an evaluation of model quality after changing attention selections.

Input: <layer>_s<sequence_length>.pt records from sfa_v1.py, or equivalent
JSON files (useful for portable fixtures). Candidate scores must be finite.
"""
from __future__ import annotations

import argparse
from collections import Counter, OrderedDict, defaultdict
import csv
import json
import math
from pathlib import Path
import re


class BatchLRU:
    def __init__(self, capacity):
        self.capacity = capacity
        self.rows = OrderedDict()

    def commit(self, selected):
        selected = set(selected)
        if len(selected) > self.capacity:
            raise ValueError("Cache cannot hold the selected attention batch")
        # Protect all selected hits while allocating misses.
        for token in selected:
            self.rows.pop(token, None)
        while len(self.rows) + len(selected) > self.capacity:
            self.rows.popitem(last=False)
        for token in sorted(selected):
            self.rows[token] = None


def choose(native, ranked_candidates, available, budget, protect_top=0):
    """Replace only scored, unprotected native misses with candidate hits.

    Native IDs absent from the reference pool have unknown rank and are
    conservatively protected. Their fraction is reported in trace metrics.
    """
    native = set(native)
    rank = {token: i for i, token in enumerate(ranked_candidates)}
    additions = [t for t in ranked_candidates if t not in native and t in available]
    removals = sorted(
        (t for t in native if t not in available and t in rank
         and rank[t] >= protect_top),
        key=lambda t: rank[t], reverse=True,
    )
    count = min(budget, len(additions), len(removals))
    return (native - set(removals[:count])) | set(additions[:count]), count


def flatten(value):
    if hasattr(value, "tolist"):
        value = value.tolist()
    if isinstance(value, list):
        return [x for item in value for x in flatten(item)]
    return [value]


def load_record(path, seq_len, *, include_scores=False):
    if path.suffix == ".pt":
        try:
            import torch
        except ImportError as exc:
            raise RuntimeError("Reading .pt needs PyTorch; run in your inference container") from exc
        raw = torch.load(path, map_location="cpu", weights_only=True)
    else:
        raw = json.loads(path.read_text())
    keys = flatten(raw["seq_lens_key"])
    queries = flatten(raw["seq_lens_query"])
    if keys != [seq_len] or queries != [1]:
        raise ValueError(f"{path}: requires a single query, single request decode")
    if raw.get("has_reference_candidates") is not True:
        raise ValueError(f"{path}: missing reference candidates; native-only traces are insufficient")
    native_list = [int(t) for t in flatten(raw["indices"]) if 0 <= int(t) < seq_len]
    native = set(native_list)
    if len(native) != min(2048, seq_len) or len(native) != len(native_list):
        raise ValueError(f"{path}: unexpected native count or duplicate native IDs")
    ids = [int(t) for t in flatten(raw["candidate_indices"])]
    scores = [float(s) for s in flatten(raw["candidate_scores"])]
    if (len(ids) != min(2400, seq_len) or len(ids) != len(scores)
            or len(set(ids)) != len(ids) or any(t < 0 or t >= seq_len for t in ids)
            or not all(math.isfinite(s) for s in scores)):
        raise ValueError(f"{path}: invalid top-2400 candidates/scores")
    # Deterministic score tie-break; native fused top-k may use a different one.
    ranked = [t for t, s in sorted(zip(ids, scores), key=lambda pair: (-pair[1], pair[0]))]
    result = (str(raw["layer"]), native, ranked)
    return (*result, dict(zip(ids, scores))) if include_scores else result


def discover(directory):
    groups = defaultdict(list)
    for path in directory.iterdir():
        match = re.fullmatch(r"(.+)_s(\d+)\.(pt|json)", path.name)
        if match:
            groups[match[1]].append((int(match[2]), path))
    if not groups:
        raise ValueError("No <layer>_s<seq_len>.pt files; pass ONE sample directory")
    for group, files in groups.items():
        files.sort()
        seqs = [s for s, _ in files]
        if any(b != a + 1 for a, b in zip(seqs, seqs[1:])):
            raise ValueError(f"{group}: duplicate or missing decode steps")
    ranges = {(files[0][0], files[-1][0], len(files)) for files in groups.values()}
    if len(ranges) != 1:
        raise ValueError("Layers do not cover the same decode steps")
    return groups


def measure(selected, cache, ready):
    hits = len(selected & cache.rows.keys())
    ready_only = len((selected - cache.rows.keys()) & ready)
    return Counter(selected=len(selected), cache_hits=hits, fresh_hits=ready_only,
                   misses=len(selected) - hits - ready_only)


def summary_row(capacity, mode, totals, row_bytes):
    selected = totals["selected"]
    return dict(capacity_rows_per_layer=capacity, strategy=mode, **dict(totals),
                cache_hit_rate=totals["cache_hits"] / selected if selected else None,
                available_hit_rate=(totals["cache_hits"] + totals["fresh_hits"]) / selected if selected else None,
                logical_miss_bytes=totals["misses"] * row_bytes)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("sample_dir", type=Path)
    p.add_argument("--capacities", nargs="+", type=int, default=[2048, 4096, 8192, 16384])
    p.add_argument("--max-replacements", type=int, default=128)
    p.add_argument("--protect-top", type=int, default=0,
                   help="Protect native IDs within this reference ranking prefix")
    p.add_argument("--warmup-steps", type=int, default=100,
                   help="Replay but exclude these first steps per layer from reported rates")
    p.add_argument("--current-token", choices=["resident", "ordinary"], default="resident")
    p.add_argument("--row-bytes", type=int, default=1152)
    p.add_argument("--expected-layers", type=int)
    p.add_argument("--out-dir", type=Path, default=Path("hbm_sim_out"))
    args = p.parse_args()
    if (min(args.capacities) < 2048 or args.max_replacements < 0
            or args.protect_top < 0 or args.warmup_steps < 0 or args.row_bytes <= 0):
        p.error("capacities >= 2048; budgets/warmup nonnegative; row bytes positive")
    groups = discover(args.sample_dir)
    if args.expected_layers is not None and len(groups) != args.expected_layers:
        p.error(f"Expected {args.expected_layers} layers, found {len(groups)}")
    if any(len(files) <= args.warmup_steps for files in groups.values()):
        p.error("warmup removes all measured steps")
    capacities = sorted(set(args.capacities))
    args.out_dir.mkdir(parents=True, exist_ok=True)
    aggregate = defaultdict(Counter)
    quality = Counter()
    fields = ["layer", "seq_len", "step", "capacity", "strategy", "warmup",
              "selected", "cache_hits", "fresh_hits", "misses", "replacements"]
    with (args.out_dir / "steps.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        seen_layers = set()
        for group, files in sorted(groups.items()):
            states = {c: (BatchLRU(c), BatchLRU(c)) for c in capacities}
            layer_name = None
            for step, (seq_len, path) in enumerate(files):
                layer, native, ranked = load_record(path, seq_len)
                if layer_name is None:
                    if layer in seen_layers:
                        raise ValueError(f"Duplicate layer groups: {layer}")
                    seen_layers.add(layer)
                    layer_name = layer
                elif layer != layer_name:
                    raise ValueError(f"Inconsistent layer metadata: {path}")
                quality.update(records=1, native_rows=len(native),
                               native_covered=len(native & set(ranked)),
                               native_ref2048_overlap=len(native & set(ranked[:len(native)])))
                ready = {seq_len - 1} if args.current_token == "resident" else set()
                for capacity, (base, aware) in states.items():
                    shadow, ns = choose(native, ranked, base.rows.keys() | ready,
                                        args.max_replacements, args.protect_top)
                    chosen, na = choose(native, ranked, aware.rows.keys() | ready,
                                        args.max_replacements, args.protect_top)
                    for mode, selection, cache, replacements in (
                        ("baseline", native, base, 0),
                        ("same_state_replacement", shadow, base, ns),
                        ("independent_replacement", chosen, aware, na),
                    ):
                        counts = measure(selection, cache, ready)
                        counts["replacements"] = replacements
                        writer.writerow(dict(layer=layer, seq_len=seq_len, step=step,
                                             capacity=capacity, strategy=mode,
                                             warmup=int(step < args.warmup_steps), **counts))
                        if step >= args.warmup_steps:
                            aggregate[capacity, mode].update(counts)
                    base.commit(native)
                    aware.commit(chosen)
            print(f"Finished {layer_name}: {len(files)} steps", flush=True)
    rows = []
    for capacity in capacities:
        base_misses = aggregate[capacity, "baseline"]["misses"]
        for mode in ("baseline", "same_state_replacement", "independent_replacement"):
            row = summary_row(capacity, mode, aggregate[capacity, mode], args.row_bytes)
            row["miss_reduction_vs_baseline"] = (
                1 - row["misses"] / base_misses if base_misses else None)
            row["persistent_cache_bytes_all_layers"] = capacity * args.row_bytes * len(groups)
            rows.append(row)
    report = dict(settings={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                  layers=len(groups), trace_quality=dict(quality), results=rows,
                  limitations=["Fixed original-model trajectory; no quality or TPOT conclusion",
                               "Cold per-layer cache; no prefill/prefix initialization",
                               "Logical row misses, not SSD physical requests/bytes",
                               "Reference FP32 candidates may differ from fused native ranking",
                               "Current KV available outside cache; admitted only if selected"])
    (args.out_dir / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    with (args.out_dir / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"Summary: {args.out_dir / 'summary.csv'}")


if __name__ == "__main__":
    main()
