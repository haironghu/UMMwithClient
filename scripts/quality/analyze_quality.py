#!/usr/bin/env python3
"""Compare run A vs run B results: task scores, agreement, first divergence,
and chosen-token logprob deltas over the shared prefix (the teacher-forced
counterfactual up to the first output difference). Pure stdlib.
"""
import argparse
import csv
import json
from pathlib import Path
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (load_dataset, match_answer, percentile,  # noqa: E402
                    read_jsonl, write_json_atomic)


def results_by_id(path):
    rows = {}
    for row in read_jsonl(path):
        if row.get("ok") and row["id"] not in rows:
            rows[row["id"]] = row
    return rows


def compare(pair_a, pair_b, dataset):
    """One joined row with scores, agreement and divergence statistics."""
    a, b = pair_a, pair_b
    example = dataset.get(a["id"], {})
    score_a = match_answer(a.get("output"), example.get("reference"),
                           example.get("match", "contain"))
    score_b = match_answer(b.get("output"), example.get("reference"),
                           example.get("match", "contain"))
    tokens_a, tokens_b = a.get("tokens") or [], b.get("tokens") or []
    text_a, text_b = (a.get("output") or "").strip(), (b.get("output") or "").strip()
    if tokens_a and tokens_b:
        agree = tokens_a == tokens_b
        divergence = next((i for i, (x, y) in enumerate(zip(tokens_a, tokens_b)) if x != y),
                          None if len(tokens_a) == len(tokens_b) else min(len(tokens_a), len(tokens_b)))
    else:
        agree = text_a == text_b
        divergence = next((i for i, (x, y) in enumerate(zip(text_a, text_b)) if x != y),
                          None if len(text_a) == len(text_b) else min(len(text_a), len(text_b)))
    logprobs_a = a.get("token_logprobs") or []
    logprobs_b = b.get("token_logprobs") or []
    deltas = []
    if divergence is None and logprobs_a and len(logprobs_a) == len(logprobs_b):
        deltas = [b_lp - a_lp for a_lp, b_lp in zip(logprobs_a, logprobs_b)
                  if a_lp is not None and b_lp is not None]
    return dict(id=a["id"], task=a.get("task") or "default",
                score_a=score_a, score_b=score_b, agree=agree,
                divergence=divergence, lp_delta_mean=statistics.fmean(deltas) if deltas else None,
                lp_delta_min=min(deltas) if deltas else None,
                lp_delta_p05=percentile(deltas, 0.05), shared=len(deltas),
                out_a=text_a[:80], out_b=text_b[:80])


def summarize(joined):
    by_task = {}
    for row in joined:
        by_task.setdefault(row["task"], []).append(row)

    def block(rows):
        def share(predicate):
            return round(sum(1 for r in rows if predicate(r)) / len(rows), 4) if rows else None
        diverged = [r["divergence"] for r in rows if r["divergence"] is not None]
        deltas = [r["lp_delta_mean"] for r in rows if r["lp_delta_mean"] is not None]
        scored_a = [r["score_a"] for r in rows if r["score_a"] is not None]
        scored_b = [r["score_b"] for r in rows if r["score_b"] is not None]
        return dict(n=len(rows),
                    score_a=round(sum(scored_a) / len(scored_a), 4) if scored_a else None,
                    score_b=round(sum(scored_b) / len(scored_b), 4) if scored_b else None,
                    agreement=share(lambda r: r["agree"]),
                    a_only_right=share(lambda r: r["score_a"] and r["score_b"] is False),
                    b_only_right=share(lambda r: r["score_b"] and r["score_a"] is False),
                    both_wrong_agreeing=share(lambda r: r["agree"] and r["score_a"] is False
                                              and r["score_b"] is False),
                    diverged_share=share(lambda r: r["divergence"] is not None),
                    first_divergence_median=percentile(diverged, 0.5),
                    lp_delta_mean=round(statistics.fmean(deltas), 5) if deltas else None,
                    lp_delta_p05=round(percentile(deltas, 0.05), 5) if deltas else None)

    summary = dict(overall=block(joined),
                   by_task={task: block(rows) for task, rows in sorted(by_task.items())},
                   regressions=[r["id"] for r in joined if r["score_a"] and r["score_b"] is False],
                   improvements=[r["id"] for r in joined if r["score_b"] and r["score_a"] is False])
    return summary


def main(args):
    dataset = {row["id"]: row for row in load_dataset(args.dataset)}
    run_a = results_by_id(args.results_a)
    run_b = results_by_id(args.results_b)
    shared = [i for i in dataset if i in run_a and i in run_b]
    only = dict(a=len([i for i in dataset if i in run_a and i not in run_b]),
                b=len([i for i in dataset if i in run_b and i not in run_a]))
    joined = [compare(run_a[i], run_b[i], dataset) for i in shared]
    summary = summarize(joined)
    summary.update(dataset_size=len(dataset), compared=len(shared), unpaired=only)
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    write_json_atomic(out / "summary.json", summary)
    with (out / "per_example.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(joined[0]) if joined else ["id"])
        writer.writeheader()
        writer.writerows(joined)
    print(json.dumps(summary["overall"], indent=2))
    print(f"summary: {out / 'summary.json'}; regressions={len(summary['regressions'])}, "
          f"improvements={len(summary['improvements'])}")
    if shared and any(r["lp_delta_mean"] is None and r["agree"] for r in joined):
        print("note: some agreeing pairs lack aligned logprobs; request --logprobs 1 "
              "in the runner to measure the teacher-forced deltas")


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dataset", required=True)
    p.add_argument("--results-a", required=True)
    p.add_argument("--results-b", required=True)
    p.add_argument("--out-dir", required=True)
    return p


if __name__ == "__main__":
    main(parser().parse_args())
