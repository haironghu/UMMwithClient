#!/usr/bin/env python3
"""Run one serial quality pass against the serving endpoint, collecting traces.

One request in flight at a time (the hook only records single-request decode),
greedy decoding, per-example marker rotation into DSA_DUMP_DIR/current_sample
(the hook routes each example into its own dump subdirectory), and chosen-token
logprobs for the later teacher-forced comparison. Results append to
results.jsonl; rerunning skips examples that already completed, so a run can
continue after interruption.

Run this twice over the SAME dataset: once against the native server (or
DSA_OVERRIDE_BUDGET=0) as run A, once with the override active as run B. The
server must be otherwise idle so decode batches stay single-request.
"""
import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (append_jsonl, auth_headers, http_json, load_dataset,  # noqa: E402
                    sanitize_id, write_json_atomic)


def prompt_tokens(endpoint, headers, model, prompt, timeout):
    payload = http_json(f"{endpoint}/tokenize", {"model": model, "prompt": prompt},
                        timeout, headers)
    tokens = payload.get("tokens") or payload.get("token_ids")
    return len(tokens) if tokens is not None else None


def completion(endpoint, headers, model, prompt, max_tokens, logprobs, timeout):
    payload = {"model": model, "prompt": prompt, "max_tokens": max_tokens,
               "temperature": 0.0}
    if logprobs:
        payload["logprobs"] = logprobs
    data = http_json(f"{endpoint}/completions", payload, timeout, headers)
    choice = data["choices"][0]
    logp = choice.get("logprobs") or {}
    usage = data.get("usage") or {}
    return dict(output=choice.get("text") or "",
                tokens=logp.get("tokens") or [],
                token_logprobs=logp.get("token_logprobs") or [],
                finish_reason=choice.get("finish_reason"),
                prompt_tokens=usage.get("prompt_tokens"),
                completion_tokens=usage.get("completion_tokens"))


def one_example(args, example, headers):
    """Return the result row; retries transient failures once."""
    max_tokens = int(example.get("max_tokens") or args.max_tokens)
    result = dict(id=example["id"], task=example.get("task") or "default",
                  prompt_chars=len(example["prompt"]), max_tokens=max_tokens,
                  dump_subdir=sanitize_id(example["id"]))
    if args.tokenize_endpoint:
        try:
            tokens = prompt_tokens(args.tokenize_endpoint, headers, args.model,
                                   example["prompt"], args.timeout)
            if tokens is not None:
                result["prompt_tokens"] = tokens
                if tokens + max_tokens > args.max_prompt_tokens:
                    result.update(ok=False, filtered=True,
                                  error=f"prompt {tokens}+{max_tokens} exceeds "
                                        f"{args.max_prompt_tokens}")
                    return result
        except Exception as exc:
            print(f"tokenize unavailable ({exc}); sending without pre-filter", flush=True)
            args.tokenize_endpoint = None
    started = time.monotonic()
    for attempt in (1, 2):
        try:
            answer = completion(args.endpoint, headers, args.model, example["prompt"],
                                max_tokens, args.logprobs, args.timeout)
            result.update(answer, ok=True, seconds=round(time.monotonic() - started, 3),
                          attempts=attempt)
            return result
        except Exception as exc:
            if attempt == 2:
                result.update(ok=False, error=f"{type(exc).__name__}: {exc}",
                              seconds=round(time.monotonic() - started, 3), attempts=2)
                return result
            print(f"{example['id']}: transient error ({exc}); retrying once", flush=True)
            time.sleep(10)


def main(args):
    if not args.tokenize_endpoint:
        # /tokenize lives on the base URL, one level above /v1.
        import re
        args.tokenize_endpoint = re.sub(r"/v1/?$", "", args.endpoint)
    dataset = load_dataset(args.dataset, args.limit)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    results = out_dir / "results.jsonl"
    done = set()
    if results.exists():
        for row in (json.loads(line) for line in results.read_text(encoding="utf-8").splitlines() if line.strip()):
            if row.get("ok"):
                done.add(row["id"])
        saved = {}
        if (out_dir / "manifest.json").exists():
            saved = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
        same = (saved.get("label") == args.label and saved.get("dataset") == str(args.dataset))
        if done and not same and not args.append_ok:
            raise SystemExit(f"{results} already holds {len(done)} completed rows from a "
                             "different label/dataset; pass --append-ok to extend anyway")
        if done:
            print(f"resuming: {len(done)} completed examples skipped", flush=True)
    marker = Path(args.dump_root) / "current_sample" if args.dump_root else None
    if marker and not Path(args.dump_root).is_dir():
        raise SystemExit(f"--dump-root is not a directory: {args.dump_root}")
    headers = auth_headers(args.api_key)
    manifest = dict(status="running", label=args.label, dataset=str(args.dataset),
                    endpoint=args.endpoint, model=args.model,
                    max_tokens=args.max_tokens, dump_root=args.dump_root,
                    logprobs=args.logprobs, started=time.time(), completed=0,
                    failed=0, filtered=0, skipped=len(done))
    write_json_atomic(out_dir / "manifest.json", manifest)
    try:
        for example in dataset:
            if example["id"] in done:
                continue
            if marker:
                marker.write_text(sanitize_id(example["id"]) + "\n", encoding="utf-8")
            row = one_example(args, example, headers)
            append_jsonl(results, row)
            key = "filtered" if row.get("filtered") else ("completed" if row["ok"] else "failed")
            manifest[key] += 1
            write_json_atomic(out_dir / "manifest.json", manifest)
            detail = row.get("output") or row.get("error")
            print(f"[{manifest['completed'] + manifest['failed']}/{len(dataset)}] "
                  f"{row['id']}: {key} in {row.get('seconds')}s "
                  f"({str(detail)[:60]!r})", flush=True)
    except KeyboardInterrupt:
        manifest["status"] = "interrupted"
        write_json_atomic(out_dir / "manifest.json", manifest)
        print("interrupted; rerun with --append-ok to continue", flush=True)
        return 130
    finally:
        if marker and marker.exists():
            marker.unlink()  # doc 17: remove so later requests cannot mix in
    manifest["status"] = "complete"
    manifest["finished"] = time.time()
    write_json_atomic(out_dir / "manifest.json", manifest)
    print(f"done: {json.dumps({k: manifest[k] for k in ('completed', 'failed', 'filtered', 'skipped')})}",
          flush=True)
    return 0


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dataset", required=True, help="normalized JSONL from convert_datasets/make_passkey_dataset")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--label", default="run", help="A/B label recorded in the manifest")
    p.add_argument("--endpoint", required=True, help="vLLM base URL, e.g. http://host:8088/v1")
    p.add_argument("--tokenize-endpoint", help="vLLM base URL for /tokenize pre-filtering; defaults to --endpoint")
    p.add_argument("--model", default="glm-5")
    p.add_argument("--api-key")
    p.add_argument("--max-tokens", type=int, default=32)
    p.add_argument("--logprobs", type=int, default=1,
                   help="top-k logprobs to request; chosen-token logprobs need >=1")
    p.add_argument("--max-prompt-tokens", type=int, default=190000)
    p.add_argument("--timeout", type=int, default=3600)
    p.add_argument("--dump-root", help="the server's DSA_DUMP_DIR (shared mount); omit to skip markers")
    p.add_argument("--limit", type=int)
    p.add_argument("--append-ok", action="store_true",
                   help="required to add examples to an out-dir that already has results")
    return p


if __name__ == "__main__":
    code = main(parser().parse_args())
    sys.exit(code if code else 0)
