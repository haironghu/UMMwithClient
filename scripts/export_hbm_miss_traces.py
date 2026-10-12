#!/usr/bin/env python3
"""Export actual miss IDs, not summary counts, for the existing SSD replay.

One sample per invocation. SQLite spools records to disk so all six policies
need not live in RAM. Output order is decode step then original numeric layer.
same_state_replacement is counterfactual: baseline cache maintenance I/O is
not included. Run baseline too; never interpret it as an online cache policy.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import sqlite3

from simulate_hbm_candidates import BatchLRU, choose, discover, load_record, measure

MODES = ("baseline", "same_state_replacement", "independent_replacement")


def export(args):
    groups = discover(args.sample_dir)
    if args.expected_layers is not None and len(groups) != args.expected_layers:
        raise ValueError(f"Expected {args.expected_layers} layers, got {len(groups)}")
    if min(args.capacities) < 2048 or args.warmup_steps < 0 or args.max_replacements < 0 or args.protect_top < 0:
        raise ValueError("Invalid cache capacity, warmup or replacement budget")
    if any(len(files) <= args.warmup_steps for files in groups.values()):
        raise ValueError("Warmup excludes every step")
    args.out_dir.mkdir(parents=True, exist_ok=False)
    manifest = dict(schema_version=1, status="running", sample_dir=str(args.sample_dir.resolve()),
                    sample_id=args.sample_dir.name, capacities=sorted(set(args.capacities)),
                    max_replacements=args.max_replacements, protect_top=args.protect_top,
                    warmup_steps=args.warmup_steps, current_token=args.current_token,
                    layers=len(groups), files=[],
                    assumptions=["fixed baseline model trajectory; token-granular per-layer batch LRU",
                                 "same_state reads exclude cost of maintaining baseline residency",
                                 "all-zero-miss batches retained; original context geometry retained"])
    manifest_path = args.out_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2))
    totals = defaultdict(Counter)
    quality = Counter()
    spool = args.out_dir / "spool.sqlite"
    db = sqlite3.connect(spool)
    try:
        db.execute("PRAGMA cache_size=-8192")
        db.execute("CREATE TABLE rows (capacity INTEGER, mode TEXT, step INTEGER, layer INTEGER, body TEXT, PRIMARY KEY(capacity,mode,step,layer))")
        seen_layers = set()
        for group, paths in sorted(groups.items()):
            states = {c: (BatchLRU(c), BatchLRU(c)) for c in manifest["capacities"]}
            expected_layer = None
            for step, (seq_len, path) in enumerate(paths):
                layer_name, native, ranked = load_record(path, seq_len)
                match = re.search(r"(?:^|\.)layers\.(\d+)(?:\.|$)", layer_name)
                if not match:
                    raise ValueError(f"Cannot extract original model layer from {layer_name}")
                layer = int(match[1])
                if expected_layer is None:
                    if layer in seen_layers:
                        raise ValueError(f"Duplicate layer {layer}")
                    expected_layer = layer
                    seen_layers.add(layer)
                if layer != expected_layer:
                    raise ValueError(f"Inconsistent layer in {path}")
                quality.update(native_rows=len(native), native_covered=len(native & set(ranked)),
                               native_ref2048_overlap=len(native & set(ranked[:len(native)])))
                ready = {seq_len - 1} if args.current_token == "resident" else set()
                for capacity, (base, aware) in states.items():
                    shadow, ns = choose(native, ranked, base.rows.keys() | ready,
                                        args.max_replacements, args.protect_top)
                    chosen, na = choose(native, ranked, aware.rows.keys() | ready,
                                        args.max_replacements, args.protect_top)
                    for mode, selected, cache, replacements in (
                        ("baseline", native, base, 0),
                        ("same_state_replacement", shadow, base, ns),
                        ("independent_replacement", chosen, aware, na),
                    ):
                        misses = sorted(selected - cache.rows.keys() - ready)
                        stats = measure(selected, cache, ready)
                        stats["replacements"] = replacements
                        warmup = step < args.warmup_steps
                        if not warmup:
                            totals[capacity, mode].update(stats)
                        record = dict(request_id=args.sample_dir.name, step_id=step,
                                      layer_id=layer, context_length=seq_len,
                                      topk_token_indices=misses, warmup=warmup,
                                      cache_capacity=capacity, cache_strategy=mode,
                                      **dict(stats))
                        db.execute("INSERT INTO rows VALUES (?,?,?,?,?)", (
                            capacity, mode, step, layer, json.dumps(record, separators=(",", ":"))))
                    base.commit(native)
                    aware.commit(chosen)
                if step % 100 == 0:
                    db.commit()
            db.commit()
            print(f"Exported layer {expected_layer}: {len(paths)} steps", flush=True)
        for capacity in manifest["capacities"]:
            for mode in MODES:
                name = f"c{capacity}-{mode}.jsonl"
                digest = hashlib.sha256()
                count = 0
                with (args.out_dir / name).open("xb") as output:
                    for (body,) in db.execute("SELECT body FROM rows WHERE capacity=? AND mode=? ORDER BY step,layer", (capacity, mode)):
                        raw = (body + "\n").encode()
                        output.write(raw)
                        digest.update(raw)
                        count += 1
                manifest["files"].append(dict(file=name, sha256=digest.hexdigest(), records=count,
                                              capacity=capacity, strategy=mode,
                                              measured_totals=dict(totals[capacity, mode])))
        manifest.update(status="complete", trace_quality=dict(quality))
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    finally:
        db.close()
    spool.unlink()
    print(f"Manifest: {manifest_path}")
    return manifest


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("sample_dir", type=Path)
    p.add_argument("--out-dir", type=Path, required=True, help="Must be new")
    p.add_argument("--capacities", type=int, nargs="+", default=[4096, 8192])
    p.add_argument("--max-replacements", type=int, default=128)
    p.add_argument("--protect-top", type=int, default=0)
    p.add_argument("--warmup-steps", type=int, default=100)
    p.add_argument("--current-token", choices=["resident", "ordinary"], default="resident")
    p.add_argument("--expected-layers", type=int)
    return p


if __name__ == "__main__":
    export(parser().parse_args())
