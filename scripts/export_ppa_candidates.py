#!/usr/bin/env python3
"""Export complete scored DSA candidates for the single-SSD MQSim oracle.

One request, ordered token slots, numeric layer order, one final padded tail.
No hardware access. Export full candidates and addresses; the replay computes
baseline HBM residency and filters actual SSD misses, rather than this exporter.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sqlite3

from simulate_hbm_candidates import discover, load_record


def export(args):
    if args.slot_bytes != 4096 or args.segment_bytes <= 0 or args.segment_bytes % 16384:
        raise ValueError('Requires 4096 B slots and a positive 16 KiB-aligned segment')
    if args.max_steps is not None and args.max_steps < 1:
        raise ValueError('max-steps must be positive')
    groups = discover(args.sample_dir)
    if args.expected_layers is not None and len(groups) != args.expected_layers:
        raise ValueError('Unexpected total layer count')
    chosen = []
    for _, files in groups.items():
        name, _, _ = load_record(files[0][1], files[0][0])
        match = re.search(r'(?:^|\.)layers\.(\d+)(?:\.|$)', name)
        if match is None:
            raise ValueError(f'Cannot parse original layer ID: {name}')
        layer = int(match[1])
        if args.layers is None or layer in args.layers:
            chosen.append((layer, files[:args.max_steps]))
    chosen.sort()
    if not chosen or len({l for l, _ in chosen}) != len(chosen):
        raise ValueError('No layers selected or duplicate layer IDs')
    if args.layers is not None and set(args.layers) != {l for l, _ in chosen}:
        raise ValueError('Requested layer missing')
    args.out_dir.mkdir(parents=True, exist_ok=False)
    manifest = dict(schema_version=2, status='running', source=str(args.sample_dir.resolve()),
                    sample_id=args.sample_dir.name, device_count=1, slot_bytes=args.slot_bytes,
                    segment_bytes=args.segment_bytes, layout='ordered_single_ssd',
                    hbm_cache_policy='computed_by_replay', current_token='directly_available_and_protected', layers=[])
    mp = args.out_dir / 'manifest.json'
    mp.write_text(json.dumps(manifest, indent=2))
    offset = 0
    with sqlite3.connect(args.out_dir / 'sort.sqlite') as db:
        db.execute('PRAGMA cache_size=-8192')
        db.execute('CREATE TABLE records (step INT, layer INT, body TEXT, PRIMARY KEY(step,layer))')
        for layer, files in chosen:
            # Include every KV up to the last exported context, not just candidates.
            context = files[-1][0]
            base = offset
            manifest['layers'].append(dict(layer_id=layer, context_length=context, base_bytes=base))
            offset += context * args.slot_bytes
            for step, (seq, path) in enumerate(files):
                name, native, ranked, scores = load_record(path, seq, include_scores=True)
                match = re.search(r'(?:^|\.)layers\.(\d+)(?:\.|$)', name)
                if match is None or int(match[1]) != layer:
                    raise ValueError(f'Inconsistent layer: {path}')
                ids = sorted(native | set(ranked))
                row = dict(sample_id=args.sample_dir.name, layer_id=layer, step_id=step,
                           context_length=seq, current_token=seq-1, native=sorted(native),
                           candidates=[dict(token=t, score=scores.get(t), device=0,
                                            offset_bytes=base+t*args.slot_bytes) for t in ids],
                           reference_count=len(ranked), native_covered=len(native & set(ranked)))
                db.execute('INSERT INTO records VALUES (?,?,?)', (step, layer, json.dumps(row, separators=(',', ':'))))
            db.commit()
            print(f'Exported layer={layer}, steps={len(files)}, context={context}', flush=True)
        digest = hashlib.sha256()
        count = 0
        with (args.out_dir / 'candidates.jsonl').open('wb') as f:
            for (body,) in db.execute('SELECT body FROM records ORDER BY step,layer'):
                data = (body+'\n').encode()
                f.write(data); digest.update(data); count += 1
    (args.out_dir / 'sort.sqlite').unlink()
    snapshot = ((offset + args.segment_bytes-1)//args.segment_bytes)*args.segment_bytes
    manifest.update(status='complete', records=count, logical_kv_bytes=offset,
                    snapshot_bytes=snapshot, candidates_sha256=digest.hexdigest(),
                    limitations=['Single SSD only: position hash degenerates to device 0',
                                 'Synthetic snapshot, no KV values or model quality evaluation',
                                 'Final exported context is prepared before reads; no online writes',
                                 'Candidate reference scores may differ from native fused ranking'])
    mp.write_text(json.dumps(manifest, indent=2)+'\n')
    return manifest


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('sample_dir', type=Path)
    p.add_argument('--out-dir', type=Path, required=True)
    p.add_argument('--layers', nargs='+', type=int)
    p.add_argument('--max-steps', type=int)
    p.add_argument('--expected-layers', type=int)
    p.add_argument('--slot-bytes', type=int, default=4096)
    p.add_argument('--segment-bytes', type=int, default=26640384)
    return p


if __name__ == '__main__':
    export(parser().parse_args())
