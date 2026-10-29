#!/usr/bin/env python3
"""Validate a dsa_dump_hook output directory against the offline replay contract.

Checks, per file and across files, everything scripts/simulate_hbm_candidates.py
::discover()/load_record() and scripts/export_ppa_candidates.py will later
require, plus faithfulness statistics of the FP32 reference ranking against the
fused native top-2048. Standalone: runs anywhere with CPU torch, no repo
imports, so it can be executed inside the inference container right after a
collection run.

Exit code 0 = the dump directory is consumable by the offline pipeline.
"""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import re
import sys

LAYER_RE = re.compile(r"(?:^|\.)layers\.(\d+)(?:\.|$)")
FILE_RE = re.compile(r"(.+)_s(\d+)\.(pt|json)")


def flatten(value):
    if hasattr(value, "tolist"):
        value = value.tolist()
    if isinstance(value, list):
        return [x for item in value for x in flatten(item)]
    return [value]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump_dir", type=Path)
    parser.add_argument("--native-topk", type=int, default=2048)
    parser.add_argument("--ref-topk", type=int, default=2400)
    parser.add_argument("--min-overlap", type=float, default=0.9,
                        help="Warn when mean native-in-reference overlap drops below this fraction")
    args = parser.parse_args()

    groups = defaultdict(list)
    for path in args.dump_dir.iterdir():
        match = FILE_RE.fullmatch(path.name)
        if match:
            groups[match[1]].append((int(match[2]), path))
    if not groups:
        sys.exit(f"No <layer>_s<seq>.pt files in {args.dump_dir}")

    errors = []
    warnings = []
    overlaps = []
    missing_native = []
    for group in sorted(groups):
        files = sorted(groups[group])
        seqs = [s for s, _ in files]
        for a, b in zip(seqs, seqs[1:]):
            if b != a + 1:
                errors.append(f"{group}: decode steps not consecutive: {a} -> {b}")
        for seq, path in files:
            if path.suffix == ".json":
                raw = json.loads(path.read_text())
            else:
                import torch

                raw = torch.load(path, map_location="cpu", weights_only=True)
            if flatten(raw["seq_lens_key"]) != [seq]:
                errors.append(f"{path.name}: seq_lens_key != [{seq}]")
            if flatten(raw["seq_lens_query"]) != [1]:
                errors.append(f"{path.name}: seq_lens_query != [1]")
            if raw.get("has_reference_candidates") is not True:
                errors.append(f"{path.name}: has_reference_candidates is not True")
            layer_field = str(raw["layer"])
            if layer_field != group:
                errors.append(f"{path.name}: layer field {layer_field!r} != filename group {group!r}")
            if not LAYER_RE.search(layer_field):
                errors.append(f"{path.name}: cannot parse layers.N from {layer_field!r}")
            native = [int(t) for t in flatten(raw["indices"])]
            expected = min(args.native_topk, seq)
            if (len(native) != expected or len(set(native)) != len(native)
                    or any(not 0 <= t < seq for t in native)):
                errors.append(f"{path.name}: invalid native set ({len(native)} ids, expected {expected})")
            ids = [int(t) for t in flatten(raw["candidate_indices"])]
            scores = [float(s) for s in flatten(raw["candidate_scores"])]
            expected = min(args.ref_topk, seq)
            if (len(ids) != expected or len(ids) != len(scores) or len(set(ids)) != len(ids)
                    or any(not 0 <= t < seq for t in ids)):
                errors.append(f"{path.name}: invalid reference candidates ({len(ids)} ids, expected {expected})")
            if any(not math.isfinite(s) for s in scores):
                errors.append(f"{path.name}: non-finite reference scores")
            if any(b > a for a, b in zip(scores, scores[1:])):
                warnings.append(f"{path.name}: candidate scores not descending")
            overlap = len(set(native) & set(ids))
            overlaps.append(overlap)
            missing_native.append(expected - overlap)

    ranges = {(files[0][0], files[-1][0]) for files in groups.values()}
    if len(ranges) != 1:
        errors.append(f"Layers cover different decode ranges: {sorted(ranges)}")
    layers = sorted({int(LAYER_RE.search(g).group(1)) for g in groups})
    mean_overlap = sum(overlaps) / len(overlaps) if overlaps else 0.0
    min_seq_seen = min((s for files in groups.values() for s, _ in files), default=0)
    expected_native = min(args.native_topk, min_seq_seen) if min_seq_seen else args.native_topk
    summary = dict(
        directory=str(args.dump_dir),
        layers=len(groups),
        layer_ids=[layers[0], layers[-1]] if layers else [],
        files=len(overlaps),
        seq_range=sorted(ranges)[0] if len(ranges) == 1 else None,
        mean_native_in_reference=round(mean_overlap, 1),
        min_native_in_reference=min(overlaps) if overlaps else 0,
        max_native_missing_from_reference=max(missing_native) if missing_native else 0,
        errors=errors,
        warnings=warnings[:20],
    )
    print(json.dumps(summary, indent=2))
    if mean_overlap < args.min_overlap * expected_native:
        warnings.append(
            f"mean overlap {mean_overlap:.1f} below {args.min_overlap:.0%} of native size; "
            "check DSA_DUMP_INDEXER_RELU / formula assumptions before trusting the reference ranking"
        )
        print("WARNING: " + warnings[-1])
    if errors:
        print(f"FAILED: {len(errors)} error(s); first 10:")
        for line in errors[:10]:
            print("  " + line)
        sys.exit(1)
    print("OK: dumps satisfy the offline replay contract")


if __name__ == "__main__":
    main()
