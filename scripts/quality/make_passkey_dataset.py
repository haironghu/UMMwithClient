#!/usr/bin/env python3
"""Generate passkey-retrieval examples at configured token lengths and depths.

Self-contained 100k+ data: no download, answers are a few tokens (small
traces), and the passkey literally appears in the prompt, so reconcile_swaps
can trace whether a regression coincided with the key tokens being swapped
out. Token sizing uses a local tokenizer when given, otherwise a
characters-per-token approximation.
"""
import argparse
import json
from pathlib import Path
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import append_jsonl, sanitize_id  # noqa: E402

# Original filler sentences: no digits, no "passkey", neutral prose.
FILLER = [
    "The tram line along the northern riverbank was rebuilt after the flood season.",
    "Local bakers start before sunrise so the market stalls smell of warm bread.",
    "A quiet reading room on the fourth floor keeps its lamps on until midnight.",
    "The observatory publishes a small bulletin whenever a comet becomes visible.",
    "Mountain railways use rack gears where the slope exceeds ordinary adhesion.",
    "The old print shop still sets headlines by hand for the summer festival.",
    "Coastal ferries adjust their timetable twice a year with the tides.",
    "The botanical garden labels its greenhouses by average humidity bands.",
    "Village wells were replaced by a piped network during the last decade.",
    "The choir rehearses in the stone chapel because the echo carries the bass.",
    "Cartographers of that era drew sea monsters where surveys were missing.",
    "The tool museum arranges its collection by the shape of the handle.",
    "Riverside benches face the water so the sunset can be watched in silence.",
    "The clockmaker winds the tower mechanism every Thursday afternoon.",
    "Shepherds in the high valley move with the grass, not the calendar.",
    "The city archive stores newspaper pages in flat boxes sorted by district.",
    "Orchard owners cover the youngest trees whenever frost is forecast.",
    "The pottery class learned that thin walls ring clearer than thick ones.",
    "A footbridge of woven bamboo connects the two halves of the island.",
    "The amateur radio club erects a temporary mast for the winter contest.",
    "Librarians rotate the display cases with the changing school terms.",
    "The hillside vineyard terraces follow seams of dark volcanic stone.",
    "Fishing families mend nets on the quay while the tide is out.",
    "The small foundry casts bronze fittings for the restored carousel.",
    "Night trains dim the corridor lights after the second station.",
    "The open-air cinema hangs its screen between two plane trees.",
    "Gliders climb along the ridge until the thermals fade in the evening.",
    "The town council meets in a hall with a very slightly sloping floor.",
    "Boatbuilders steam the ribs of oak until they bend to the template.",
    "The weather station notes the first frost on a brass plaque by the gate.",
]


def token_counter(tokenizer=None, chars_per_token=4.0):
    """Return measure(text)->tokens: real tokenizer when available, else chars/N."""
    if tokenizer is None:
        return lambda text: max(1, round(len(text) / chars_per_token))
    return lambda text: len(tokenizer(text, add_special_tokens=False)["input_ids"])


def build_prompt(rng, measure, target_tokens, depth, digits, needles):
    keys = [str(rng.randrange(10 ** (digits - 1), 10 ** digits)) for _ in range(needles)]
    head = ("There is some text below. The text contains one or more special magic "
            "numbers for passkey retrieval. Read the text and answer the final "
            "question about them. ")
    tail = ("What is the special magic number for passkey retrieval mentioned in "
            "the text? The special magic number for passkey retrieval mentioned "
            "in the text is")
    needle_texts = [
        f" One of the special magic numbers for passkey retrieval is: {key}. Remember it. "
        for key in keys]
    quota = target_tokens - measure(head) - measure(tail) - sum(measure(n) for n in needle_texts)
    sentences, spent = [], 0
    while spent < quota:
        sentence = FILLER[rng.randrange(len(FILLER))]
        sentences.append(sentence)
        spent += measure(sentence + " ")
    # Trim the final sentence word-wise into the tolerance window.
    while sentences and spent - quota > max(8, target_tokens // 50):
        words = sentences[-1].split(" ")
        if len(words) <= 3:
            sentences.pop()
            spent -= measure(" ".join(words) + " ")
            continue
        cut = " ".join(words[:-1])
        new_spent = spent - measure(sentences[-1] + " ") + measure(cut + " ")
        sentences[-1], spent = cut, new_spent
    positions = [int(round(depth * len(sentences))) for _ in needle_texts]
    for position, needle in zip(positions, needle_texts):
        sentences.insert(min(position, len(sentences)), needle.strip())
    return head + " ".join(sentences) + " " + tail, keys


def generate(args):
    rng = random.Random(args.seed)
    tokenizer = None
    if args.tokenizer:
        from transformers import AutoTokenizer
        tokenizer = AutoTokenizer.from_pretrained(args.tokenizer, trust_remote_code=True)
    measure = token_counter(tokenizer, args.chars_per_token)
    depths = [float(d) for d in args.depths.split(",")]
    out = Path(args.out)
    if out.exists():
        raise SystemExit(f"output exists: {out}")
    out.parent.mkdir(parents=True, exist_ok=True)
    for index in range(args.count):
        depth = depths[index % len(depths)]
        prompt, keys = build_prompt(rng, measure, args.target_tokens, depth,
                                    args.digits, args.needle_count)
        row = dict(id=sanitize_id(f"passkey{args.target_tokens}_d{int(depth*100)}_{index:04d}"),
                   task=f"passkey{args.target_tokens}",
                   prompt=prompt, reference=keys[0] if args.needle_count == 1 else keys,
                   gold_text=keys[0], max_tokens=args.max_tokens, match="contain",
                   depth=depth, approx_tokens=measure(prompt))
        append_jsonl(out, row)
        print(f"{row['id']}: ~{row['approx_tokens']} tokens, depth={depth}, key={keys[0]}", flush=True)


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", required=True, help="output normalized JSONL (must be new)")
    p.add_argument("--count", type=int, default=20)
    p.add_argument("--target-tokens", type=int, default=131072)
    p.add_argument("--depths", default="0.05,0.3,0.55,0.8,0.95",
                   help="comma-separated insertion depths as fractions, cycled")
    p.add_argument("--digits", type=int, default=5)
    p.add_argument("--needle-count", type=int, default=1)
    p.add_argument("--max-tokens", type=int, default=32)
    p.add_argument("--seed", type=int, default=2026)
    p.add_argument("--tokenizer", help="local model dir; omit to approximate by chars")
    p.add_argument("--chars-per-token", type=float, default=4.0)
    return p


if __name__ == "__main__":
    generate(parser().parse_args())
