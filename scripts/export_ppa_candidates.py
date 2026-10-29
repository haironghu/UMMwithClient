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
        name, _, _, raw = load_record(files[0][1], files[0][0], include_scores=False, include_raw=True)
        match = re.search(r'(?:^|\.)layers\.(\d+)(?:\.|$)', name)
        if match is None:
            raise ValueError(f'Cannot parse original layer ID: {name}')
        layer = int(match[1])
        if args.layers is None or layer in args.layers:
            chosen.append((layer, files[:args.max_steps], raw))
    chosen.sort()
    if not chosen or len({l for l, _, _ in chosen}) != len(chosen):
        raise ValueError('No layers selected or duplicate layer IDs')
    if args.layers is not None and set(args.layers) != {l for l, _, _ in chosen}:
        raise ValueError('Requested layer missing')
    # Second-generation dumps carry their own declared-stride layout (written
    # by the online override hook before the run started); first-generation
    # dumps keep the cumulative layout from each layer's final context.
    record_strides = {r.get('stride_tokens') for _, _, r in chosen if r.get('stride_tokens') is not None}
    if len(record_strides) > 1 or (record_strides and args.declared_stride_tokens
                                   and record_strides != {args.declared_stride_tokens}):
        raise ValueError('Inconsistent stride_tokens across records (or vs --declared-stride-tokens)')
    stride = record_strides.pop() if record_strides else args.declared_stride_tokens
    applied_count = 0
    args.out_dir.mkdir(parents=True, exist_ok=False)
    manifest = dict(schema_version=3, status='running', source=str(args.sample_dir.resolve()),
                    sample_id=args.sample_dir.name, device_count=1, slot_bytes=args.slot_bytes,
                    segment_bytes=args.segment_bytes, layout='ordered_single_ssd',
                    declared_stride_tokens=stride or None,
                    hbm_cache_policy='computed_by_replay', current_token='directly_available_and_protected', layers=[])
    override_marker = args.sample_dir / '_override_config.json'
    if override_marker.is_file():
        manifest['online_override'] = json.loads(override_marker.read_text())
    mp = args.out_dir / 'manifest.json'
    mp.write_text(json.dumps(manifest, indent=2))
    offset = 0
    ranks = {}
    db = sqlite3.connect(args.out_dir / 'sort.sqlite')
    try:
        db.execute('PRAGMA cache_size=-8192')
        db.execute('CREATE TABLE records (step INT, layer INT, body TEXT, PRIMARY KEY(step,layer))')
        for rank, (layer, files, first_raw) in enumerate(chosen):
            record_driven = first_raw.get('stride_tokens') is not None
            if stride:
                layer_rank = first_raw.get('layer_rank', rank)
                if layer_rank in ranks.values():
                    raise ValueError(f'Duplicate layer_rank {layer_rank}')
                ranks[layer] = layer_rank
                base = layer_rank * stride * args.slot_bytes
                # Cover the full declared region so the prepare mapping stays
                # valid for any online run that stops within stride tokens.
                offset = max(offset, (layer_rank + 1) * stride * args.slot_bytes)
            else:
                base = offset
                offset += files[-1][0] * args.slot_bytes
            # Include every KV up to the last exported context, not just candidates.
            context = files[-1][0]
            manifest['layers'].append(dict(layer_id=layer, context_length=context, base_bytes=base))
            for step, (seq, path) in enumerate(files):
                name, native, ranked, scores, raw = load_record(path, seq, include_scores=True, include_raw=True)
                match = re.search(r'(?:^|\.)layers\.(\d+)(?:\.|$)', name)
                if match is None or int(match[1]) != layer:
                    raise ValueError(f'Inconsistent layer: {path}')
                if record_driven:
                    if raw.get('stride_tokens') != stride or raw.get('layer_rank') != ranks[layer]:
                        raise ValueError(f'Inconsistent stride/rank mid-layer: {path}')
                ids = sorted(native | set(ranked))
                row = dict(sample_id=args.sample_dir.name, layer_id=layer, step_id=step,
                           context_length=seq, current_token=seq-1, native=sorted(native),
                           candidates=[dict(token=t, score=scores.get(t), device=0,
                                            offset_bytes=base+t*args.slot_bytes) for t in ids],
                           reference_count=len(ranked), native_covered=len(native & set(ranked)))
                if 'selected' in raw:
                    if raw.get('overridden') is not True:
                        raise ValueError(f'{path}: override fell back to native; online run is not '
                                         'uniformly overridden and cannot be replayed as applied')
                    if len(raw['selected']) != len(native) or raw.get('cache_mode') != 'shared':
                        raise ValueError(f'{path}: invalid applied selection fields')
                    row.update(applied_selected=raw['selected'],
                               applied_swaps=[[o, n] for o, n in raw['swaps']],
                               applied_cache_state_sha256=raw['cache_state_sha256'])
                    applied_count += 1
                db.execute('INSERT INTO records VALUES (?,?,?)', (step, layer, json.dumps(row, separators=(',', ':'))))
            db.commit()
            print(f'Exported layer={layer}, steps={len(files)}, context={context}', flush=True)
        digest = hashlib.sha256()
        count = 0
        with (args.out_dir / 'candidates.jsonl').open('wb') as f:
            for (body,) in db.execute('SELECT body FROM records ORDER BY step,layer'):
                data = (body+'\n').encode()
                f.write(data); digest.update(data); count += 1
    finally:
        db.close()  # close even on failure; Windows cannot unlink an open file
    (args.out_dir / 'sort.sqlite').unlink()
    if applied_count and applied_count != count:
        raise ValueError('Applied selections present on only some records; mixed traces cannot be replayed')
    manifest['has_applied_selections'] = applied_count > 0
    snapshot = ((offset + args.segment_bytes-1)//args.segment_bytes)*args.segment_bytes
    manifest.update(status='complete', records=count, logical_kv_bytes=offset,
                    snapshot_bytes=snapshot, candidates_sha256=digest.hexdigest(),
                    limitations=['Single SSD only: position hash degenerates to device 0',
                                 'Synthetic snapshot, no KV values or model quality evaluation',
                                 'Final exported context is prepared before reads; no online writes',
                                 'Candidate reference scores may differ from native fused ranking'])
    if manifest['has_applied_selections']:
        manifest['limitations'].append(
            'Applied selections come from the online hook; native baseline is the same diverged trajectory')
    mp.write_text(json.dumps(manifest, indent=2) + '\n')
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
    p.add_argument('--declared-stride-tokens', type=int, default=0,
                   help='v2 layout: fixed per-layer stride in tokens (prepare mapping contract); '
                        '0 keeps the cumulative layout from each layer final context')
    return p


if __name__ == '__main__':
    export(parser().parse_args())
