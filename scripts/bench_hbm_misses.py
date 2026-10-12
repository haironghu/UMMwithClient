#!/usr/bin/env python3
"""Sequential six-case SSD-only replay; default is dry-run (no device access)."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--traces-dir", type=Path, required=True)
    p.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument("--devices-config", type=Path)
    p.add_argument("--out-dir", type=Path, required=True, help="Must be new")
    p.add_argument("--workers", type=int, default=16)
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--segment-bytes", type=int, default=26640384)
    p.add_argument("--seed", type=int, default=2026)
    p.add_argument("--allow-device-write", action="store_true",
                   help="Execute snapshot preparation writes in the configured windows; default only plans")
    args = p.parse_args()
    if args.workers < 1 or args.repeats < 1:
        p.error("workers and repeats must be positive")
    repo = args.repo.resolve()
    config = (args.devices_config or repo / "bmpclient/bench/eight_devices.huawei.json").resolve()
    manifest = json.loads((args.traces_dir / "manifest.json").read_text())
    if manifest.get("status") != "complete":
        p.error("Trace export did not complete")
    cases = manifest["files"]
    expected = {(c, s) for c in (4096, 8192) for s in
                ("baseline", "same_state_replacement", "independent_replacement")}
    if len(cases) != 6 or {(x['capacity'], x['strategy']) for x in cases} != expected:
        p.error("Expected six cases: 4096/8192 x baseline/same-state/independent")
    for case in cases:
        path = (args.traces_dir / case['file']).resolve()
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1 << 20), b''):
                digest.update(block)
        if digest.hexdigest() != case['sha256']:
            raise ValueError(f"Trace digest mismatch: {path}")
    args.out_dir.mkdir(parents=True, exist_ok=False)
    rows = []
    reference_mapping = None
    reference_library = None
    cycles = args.repeats if args.allow_device_write else 1
    for repeat in range(cycles):
        # Reverse case order on alternate repeats to expose order/cache effects.
        for case in cases if repeat % 2 == 0 else list(reversed(cases)):
            stem = f"c{case['capacity']}-{case['strategy']}-r{repeat}"
            output = (args.out_dir / f"{stem}.json").resolve()
            cmd = [sys.executable, '-u', str(repo / 'bmpclient/scripts/bench_layout_replay.py'),
                   '--trace', str((args.traces_dir / case['file']).resolve()),
                   '--devices-config', str(config), '--output', str(output),
                   '--miss-trace', '--mode', 'snapshot', '--experiment', 'layout-concurrency',
                   '--placement', 'hash', '--layouts', 'ordered',
                   '--segments', str(args.segment_bytes), '--worker-sweep', str(args.workers),
                   '--repeats', '1', '--seed', str(args.seed),
                   '--allow-device-write' if args.allow_device_write else '--dry-run']
            log_path = args.out_dir / f"{stem}.log"
            print(f"{'RUN' if args.allow_device_write else 'PLAN'} {stem}; log={log_path}", flush=True)
            with log_path.open('x') as log:
                proc = subprocess.run(cmd, cwd=repo, stdout=log, stderr=subprocess.STDOUT)
            if proc.returncode:
                raise RuntimeError(f"Case failed ({proc.returncode}); inspect {log_path}")
            if not args.allow_device_write:
                continue
            result = json.loads(output.read_text())
            if result['status'] != 'complete' or len(result['trials']) != 1:
                raise RuntimeError(f"Incomplete benchmark: {output}")
            trial = result['trials'][0]
            measured_misses = case['measured_totals']['misses']
            if trial['measured_miss_rows'] != measured_misses:
                raise RuntimeError("Measured read rows differ from cache replay")
            # Layout-concurrency disables read merging: precisely one 4KiB slot per miss.
            if trial['measured_read_bytes'] != measured_misses * 4096:
                raise RuntimeError("Submitted disk bytes differ from exported miss slots")
            if reference_mapping is None:
                reference_mapping = trial['mapping_sha256']
                reference_library = result['library_sha256']
            if trial['mapping_sha256'] != reference_mapping or result['library_sha256'] != reference_library:
                raise RuntimeError("Cases did not use identical row mapping/native library")
            rows.append(dict(capacity=case['capacity'], strategy=case['strategy'], repeat=repeat,
                             workers=args.workers, measured_miss_rows=measured_misses,
                             read_seconds=trial['active_fetch_seconds'],
                             submitted_read_bytes=trial['measured_read_bytes'],
                             useful_kv_MiB_s=trial['effective_unique_kv_MiB_s'],
                             submitted_read_MiB_s=trial['submitted_read_MiB_s'],
                             layer_p50_us=trial['fetch_us']['p50'],
                             layer_p95_us=trial['fetch_us']['p95'],
                             layer_p99_us=trial['fetch_us']['p99'],
                             step_p50_us=trial['decode_step_read_us']['p50'],
                             step_p95_us=trial['decode_step_read_us']['p95'],
                             step_p99_us=trial['decode_step_read_us']['p99'],
                             zero_miss_batches=trial['zero_miss_batches'],
                             result_file=str(output)))
            # Persist progress after every case, so completed runs survive interruption.
            with (args.out_dir / 'runs.csv').open('w', newline='') as f:
                writer = csv.DictWriter(f, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
    if not args.allow_device_write:
        print('Planning complete. No device was opened. Inspect the six logs; execution needs --allow-device-write and a new --out-dir.')
        return
    summary = []
    for case in cases:
        subset = [r for r in rows if (r['capacity'], r['strategy']) == (case['capacity'], case['strategy'])]
        base = [r for r in rows if r['capacity'] == case['capacity'] and r['strategy'] == 'baseline']
        sec = statistics.median(r['read_seconds'] for r in subset)
        base_sec = statistics.median(r['read_seconds'] for r in base)
        summary.append(dict(capacity=case['capacity'], strategy=case['strategy'],
                            median_read_seconds=sec,
                            read_time_reduction=1 - sec / base_sec if base_sec else None,
                            read_speedup=base_sec / sec if sec else None,
                            median_useful_kv_MiB_s=statistics.median(r['useful_kv_MiB_s'] for r in subset) if sec else None,
                            median_submitted_read_MiB_s=statistics.median(r['submitted_read_MiB_s'] for r in subset) if sec else None,
                            median_step_p95_us=statistics.median(r['step_p95_us'] for r in subset),
                            misses=case['measured_totals']['misses']))
    with (args.out_dir / 'comparison.csv').open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    print(f"Comparison: {args.out_dir / 'comparison.csv'}")


if __name__ == '__main__':
    main()
