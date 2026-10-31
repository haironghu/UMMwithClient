"""Shared helpers for the online quality-collection tools. Standard library only.

The sample-id sanitizer MUST stay identical to dsa_dump_hook.SAMPLE_RE: the
runner writes marker ids, the hook derives dump subdirectories from them, and
reconcile_swaps later maps results back to those directories.
"""
import json
import os
import re
import time
import urllib.error
import urllib.request
from pathlib import Path

# Same pattern as dsa_dump_hook.SAMPLE_RE (kept in sync deliberately).
SAFE_RE = re.compile(r"[^A-Za-z0-9._-]+")


def sanitize_id(text):
    cleaned = SAFE_RE.sub("_", str(text).strip())[:64]
    return cleaned or "sample"


def read_jsonl(path):
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if line:
                yield json.loads(line)


def append_jsonl(path, row):
    with open(path, "a", encoding="utf-8") as stream:
        stream.write(json.dumps(row, ensure_ascii=False) + "\n")
        stream.flush()


def write_json_atomic(path, payload):
    tmp = Path(str(path) + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def http_json(url, payload, timeout, headers=None):
    """POST JSON; raises urllib errors/timeouts for the caller to retry."""
    request = urllib.request.Request(
        url, data=json.dumps(payload).encode(), method="POST",
        headers={"Content-Type": "application/json", **(headers or {})})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode())


def auth_headers(api_key):
    return {"Authorization": f"Bearer {api_key}"} if api_key else None


def normalize_answer(text):
    return " ".join(str(text or "").split()).strip().lower()


# Case-sensitive on purpose: a lowercase leading "a bit of ..." is an article,
# not option A. The explicit-keyword pattern below stays case-insensitive.
OPTION_START_RE = re.compile(r"^\W*[\(\[]?([ABCD])(?![A-Za-z])")
OPTION_EXPLICIT_RE = re.compile(
    r"(?:answer|option|答案|选)(?:\s+is)?\s*[:\-：]?\s*[\(\[]?\s*([ABCD])\b", re.IGNORECASE)


def extract_option(output):
    """The A-D letter the model committed to: an explicit "answer is X /
    option is X / 答案：X / 选X" pattern wins (it is the conclusion), else a
    leading uppercase letter ("C.", "(D)", "B because ..."). Mid-sentence
    standalone letters and lowercase articles are deliberately ignored.
    None when nothing parseable (counted wrong, the conservative choice)."""
    text = str(output or "").strip()
    match = OPTION_EXPLICIT_RE.search(text) or OPTION_START_RE.search(text)
    return match.group(1).upper() if match else None


def match_answer(output, reference, mode="contain"):
    """None when unscoreable (no reference); True/False otherwise.

    mode "option": MCQ judgement by letter extraction; single-letter containment
    would misfire on sentences that merely contain the letter, so it is not used.
    """
    refs = [reference] if isinstance(reference, str) else list(reference or [])
    refs = [r for r in refs if str(r).strip()]
    if not refs:
        return None
    out = normalize_answer(output)
    if mode == "exact":
        return any(normalize_answer(r) == out for r in refs)
    if mode == "option":
        gold = str(refs[0]).strip().strip("()[]").upper()
        return gold in "ABCD" and extract_option(out) == gold
    return any(normalize_answer(r) in out for r in refs)


def references_of(example):
    return [example["reference"]] if isinstance(example.get("reference"), str) \
        else list(example.get("reference") or [])


def load_dataset(path, limit=None):
    """Read the normalized dataset, validating the contract the tools share."""
    rows, seen = [], set()
    for row in read_jsonl(path):
        for key in ("id", "prompt"):
            if not str(row.get(key) or "").strip():
                raise ValueError(f"dataset row missing {key}: {row}")
        safe = sanitize_id(row["id"])
        if safe in seen:
            raise ValueError(f"dataset id sanitizes to a duplicate: {row['id']} -> {safe}")
        seen.add(safe)
        if "max_tokens" in row and int(row["max_tokens"]) < 1:
            raise ValueError(f"bad max_tokens for {row['id']}")
        rows.append(row)
        if limit is not None and len(rows) >= limit:
            break
    if not rows:
        raise ValueError(f"no examples in {path}")
    return rows


def gold_text_of(example):
    """Salient text expected to literally appear in the prompt (reconciliation)."""
    explicit = example.get("gold_text")
    if explicit:
        return str(explicit)
    for ref in references_of(example):
        if ref in example["prompt"]:
            return ref
    return None


def percentile(values, ratio):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * ratio
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def monotonic():
    return time.monotonic()
