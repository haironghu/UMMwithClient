#!/usr/bin/env python3
"""Reconcile quality regressions against the online override's swap log.

For examples where run B lost an answer run A had (and for a control group
that kept it), locate the gold answer's token ids inside the prompt, then walk
that example's second-generation dump records and count how often the online
selection dropped gold tokens: present in native top-2048 but absent from the
applied selection, or explicitly swapped out. A regression whose gold tokens
were swapped out at the failing steps is direct evidence the PPA replacement
caused the loss; regressions without gold swaps point at attention-margin
effects instead.

Needs the model tokenizer locally (offset mapping) and torch for .pt dumps
(both exist on the inference machine); .json dumps are read without torch.
"""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (gold_text_of, load_dataset, match_answer,  # noqa: E402
                    read_jsonl, sanitize_id, write_json_atomic)


def find_gold_token_ids(tokenizer, prompt, needle):
    """Token ids covering every occurrence of needle in prompt (offset-based)."""
    encoded = tokenizer(prompt, add_special_tokens=False, return_offsets_mapping=True)
    ids = encoded["input_ids"]
    offsets = encoded["offset_mapping"]
    found, start = set(), 0
    while True:
        position = prompt.find(needle, start)
        if position < 0:
            return found
        end = position + len(needle)
        found.update(token for token, (a, b) in zip(ids, offsets) if b > position and a < end)
        start = position + 1


def iter_dump_records(dump_dir):
    """Yield (seq, layer, record) for a sample's dump directory."""
    for path in sorted(dump_dir.iterdir()):
        if path.suffix == ".json":
            record = json.loads(path.read_text(encoding="utf-8"))
        elif path.suffix == ".pt":
            import torch
            record = torch.load(path, map_location="cpu", weights_only=True)
        else:
            continue
        yield record.get("seq_lens_key", [None])[0], record.get("layer"), record


def reconcile_example(dump_root, example, tokenizer):
    gold_text = gold_text_of(example)
    if gold_text is None:
        return None, "gold text not found verbatim in prompt"
    dump_dir = Path(dump_root) / sanitize_id(example["id"])
    if not dump_dir.is_dir():
        return None, f"no dump directory {dump_dir}"
    gold = find_gold_token_ids(tokenizer, example["prompt"], gold_text)
    if not gold:
        return None, "gold tokens not found via offsets"
    stats = dict(gold_tokens=len(gold), records=0, gold_in_native_records=0,
                 gold_swapped_out=0, min_gold_in_selected=None,
                 worst_step=None, swapped_records=0)
    for seq, layer, record in iter_dump_records(dump_dir):
        if "selected" not in record:
            continue
        native = set(record.get("indices") or [])
        selected = set(record.get("selected") or [])
        swaps = record.get("swaps") or []
        in_native = len(gold & native)
        in_selected = len(gold & selected)
        stats["records"] += 1
        stats["gold_in_native_records"] += int(in_native > 0)
        stats["swapped_records"] += int(bool(swaps))
        stats["gold_swapped_out"] += sum(1 for old, _ in swaps if old in gold)
        if in_native and (stats["min_gold_in_selected"] is None
                          or in_selected < stats["min_gold_in_selected"]):
            stats["min_gold_in_selected"] = in_selected
            stats["worst_step"] = seq
    if not stats["records"]:
        return None, "no second-generation records (not overridden?)"
    stats["gold_ever_absent"] = int(stats["min_gold_in_selected"] == 0)
    return stats, None


def main(args):
    dataset = {row["id"]: row for row in load_dataset(args.dataset)}
    run_a = {row["id"]: row for row in read_jsonl(args.results_a) if row.get("ok")}
    run_b = {row["id"]: row for row in read_jsonl(args.results_b) if row.get("ok")}

    def scored(row_id):
        example = dataset[row_id]
        return (match_answer(run_a[row_id].get("output"), example.get("reference"),
                             example.get("match", "contain")),
                match_answer(run_b[row_id].get("output"), example.get("reference"),
                             example.get("match", "contain")))

    paired = [i for i in dataset if i in run_a and i in run_b]
    regressions = [i for i in paired if (lambda s: s[0] and s[1] is False)(scored(i))]
    controls = [i for i in paired if (lambda s: s[0] and s[1])(scored(i))][:args.controls]
    from transformers import AutoTokenizer
    tokenizer = AutoTokenizer.from_pretrained(args.tokenizer, trust_remote_code=True)

    report = {}
    for group, ids in (("regressions", regressions), ("controls", controls)):
        entries = []
        for row_id in ids:
            stats, error = reconcile_example(args.dump_root, dataset[row_id], tokenizer)
            entries.append(dict(id=row_id, **(stats or {"error": error})))
        report[group] = entries
        with_gold = [e for e in entries if e.get("gold_ever_absent")]
        swapped = [e for e in entries if e.get("gold_swapped_out")]
        report[f"{group}_stats"] = dict(n=len(entries),
                                        gold_absent_at_least_one_step=len(with_gold),
                                        gold_swapped_out_records=len(swapped))
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    write_json_atomic(out / "reconcile.json", report)
    for group in ("regressions", "controls"):
        print(f"{group}: {json.dumps(report[f'{group}_stats'])}")
    print(f"report: {out / 'reconcile.json'}")


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dataset", required=True)
    p.add_argument("--results-a", required=True)
    p.add_argument("--results-b", required=True)
    p.add_argument("--dump-root", required=True, help="the server's DSA_DUMP_DIR")
    p.add_argument("--tokenizer", required=True, help="local model dir for AutoTokenizer")
    p.add_argument("--controls", type=int, default=20)
    p.add_argument("--out-dir", required=True)
    return p


if __name__ == "__main__":
    main(parser().parse_args())
