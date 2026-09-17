"""Replay SparseKVStore/VirtualMedia on a file or an explicitly authorized block window.

The small bridge implements UMMLib's buffer/extent interface with the native
SSD pool. It does not implement RPC, metadata service or the NPU data plane.
"""
import argparse
import ctypes
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import stat
import statistics
import time
from types import SimpleNamespace

from bmpclient.bench.single_ssd import DirectPool, PAGE, KV_BYTES, load_trace, pattern, percentiles, size_arg
from bmpclient.sparse_kv import SparseKVStore, KVBlockRef
from bmpclient.sparse_kv.store import FLAG_DISK
from bmpclient.umm_client import ChunkDescriptor, UMM_TIER_SSD
from bmpclient.virtual_media import VirtualMedia
from bmpclient.bench.multi_device import (OriginalLayerHash, hash_capacities, load_devices,
                                        LayerPlacement, layer_capacities)


class DataMismatchError(RuntimeError):
    def __init__(self, details):
        self.details = details
        super().__init__('data mismatch: ' + json.dumps(details, sort_keys=True))


def diagnose_mismatch(client, store, dense_layer, record, record_index, token,
                      actual, expected, segment, shuffled, workers):
    """Capture evidence before cleanup; retry one native read, never rewrite data."""
    device = store.media.locate((dense_layer, token))
    offset, _ = store.slot_table.locate(dense_layer, token)
    physical = store.media.extent_base(device) + offset
    virtual = client.pool.reservations[device][0] + physical - client.window_bases[device]

    def describe(data):
        return dict(sha256=hashlib.sha256(data).hexdigest(), head_hex=bytes(data[:32]).hex(),
                    all_zero=not any(data))

    details = dict(request_id=record['request_id'], layer_id=record['layer_id'],
                   step_id=record['step_id'], record_index=record_index, token=token,
                   layout='shuffled' if shuffled else 'ordered', workers=workers,
                   segment_bytes=segment, device_idx=device, extent_offset=offset,
                   device_byte_offset=physical, pool_byte_offset=virtual,
                   first_mismatch_byte=next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                   expected=describe(expected), actual=describe(actual))
    retry = bytearray(PAGE)
    owner = (ctypes.c_char * PAGE).from_buffer(retry)
    try:
        client.pool.read(virtual, PAGE, ctypes.addressof(owner))
        details['native_retry'] = dict(describe(retry), matches_expected=retry == expected,
                                       matches_first=retry == actual)
    except Exception as exc:
        details['native_retry'] = dict(error=str(exc))
    return details


class PoolClient:
    """Benchmark-only device bridge; all extents use the native bitmap."""
    def __init__(self, pool, watch=None):
        self.pool = pool
        self.watch = watch
        self.watch_expected = None
        self.watch_events = []
        self.watch_context = {}
        self.write_call_us = []
        fn = pool.lib.ssd_pool_free
        fn.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64]
        fn.restype = ctypes.c_int
        # DirectPool initially reserves the full window. Return it so each VM
        # can allocate and free its own extent through the native allocator.
        for off, size in pool.reservations:
            pool.check(fn(pool.pool, off, size))
        self.live = {}
        self.next_id = 1
        self.window_bases = [pool.stats(device_idx=d)['window_base'] for d in range(pool.device_count)]

    def check_watch(self, stage):
        if self.watch is None:
            return
        device, physical = self.watch
        event = dict(self.watch_context, stage=stage, device_idx=device, device_byte_offset=physical)
        if self.watch_expected is None:
            event['status'] = 'not_written'
        else:
            virtual = self.pool.reservations[device][0] + physical - self.window_bases[device]
            data = bytearray(PAGE)
            owner = (ctypes.c_char * PAGE).from_buffer(data)
            self.pool.read(virtual, PAGE, ctypes.addressof(owner))
            expected = self.watch_expected
            event.update(status='matched' if data == expected else 'mismatch',
                         expected_sha256=hashlib.sha256(expected).hexdigest(),
                         actual_sha256=hashlib.sha256(data).hexdigest(),
                         expected_all_zero=not any(expected), actual_all_zero=not any(data))
        self.watch_events.append(event)
        print('  write-watch: ' + json.dumps(event, sort_keys=True), flush=True)
        if event['status'] == 'mismatch':
            raise DataMismatchError(event)

    def get_topology(self):
        return SimpleNamespace(num_resources=self.pool.device_count, resources=[
            SimpleNamespace(tier=UMM_TIER_SSD, online=True) for _ in range(self.pool.device_count)])

    def alloc_on_device(self, size, tier, device_idx):
        if tier != UMM_TIER_SSD or not 0 <= device_idx < self.pool.device_count:
            raise ValueError("invalid SSD device index")
        off = ctypes.c_uint64()
        self.pool.check(self.pool.lib.ssd_pool_alloc_on_device(self.pool.pool, device_idx, size, ctypes.byref(off)))
        desc = ChunkDescriptor(self.next_id, off.value, size)
        self.live[self.next_id] = (off.value, size)
        self.next_id += 1
        return desc

    def free(self, desc):
        off, size = self.live[desc.chunk_id]
        self.pool.check(self.pool.lib.ssd_pool_free(self.pool.pool, off, size))
        del self.live[desc.chunk_id]

    def resolve_ssd_extent_base(self, desc, device_idx):
        off, size = self.live[desc.chunk_id]
        base, capacity = self.pool.reservations[device_idx]
        if off < base or off + size > base + capacity:
            raise ValueError('extent does not belong to expected device')
        return self.window_bases[device_idx] + off - base

    def _io(self, desc, offset, buf, write):
        view = memoryview(buf).cast('B')
        off, size = self.live[desc.chunk_id]
        if offset < 0 or offset > size or view.nbytes > size - offset:
            raise ValueError("extent I/O out of bounds")
        # Real VM supplies writable bytearrays. Native backend handles alignment;
        # its bounce_bytes are included in the report, never hidden.
        owner = (ctypes.c_char * view.nbytes).from_buffer(view)
        if write:
            begin = time.perf_counter_ns()
            self.pool.write(off + offset, view.nbytes, ctypes.addressof(owner))
            self.write_call_us.append((time.perf_counter_ns() - begin) / 1e3)
        else:
            self.pool.read(off + offset, view.nbytes, ctypes.addressof(owner))
        if write and self.watch is not None:
            device, physical = self.watch
            virtual = self.pool.reservations[device][0] + physical - self.window_bases[device]
            local = virtual - (off + offset)
            if 0 <= local and local + PAGE <= view.nbytes:
                self.watch_expected = bytes(view[local:local + PAGE])
                self.check_watch('immediately_after_write')
        return view.nbytes

    def read_into(self, desc, offset, buf):
        return self._io(desc, offset, buf, False)

    def write_from(self, desc, offset, buf):
        return self._io(desc, offset, buf, True)


def geometry(trace, segment, mode):
    """Conservative capacity per request including padding at every flush."""
    lengths, slots, steps = {}, {}, {}
    for r in trace.records:
        key = (r['request_id'], r['layer_id'])
        if mode == 'online' and r['step_id'] < steps.get(r['request_id'], 0):
            raise ValueError('online step_id must not decrease within a request')
        steps[r['request_id']] = r['step_id']
        old = lengths.get(key, 0)
        new = r['context_length']
        if mode == 'online' and new < old:
            raise ValueError(f"online context shrank for {key}; use a new request_id for a new lifecycle")
        if new > old:
            delta = new - old
            slots[r['request_id']] = slots.get(r['request_id'], 0) + ((delta * PAGE + segment - 1) // segment) * segment
        lengths[key] = max(old, new)
    if mode == 'snapshot':
        slots = {}
        for (request, _), count in lengths.items():
            slots[request] = slots.get(request, 0) + count * PAGE
        slots = {key: ((value + segment - 1) // segment) * segment for key, value in slots.items()}
    return lengths, slots


def native_stats(pool):
    devices = [pool.stats(device_idx=d) for d in range(pool.device_count)]
    return dict({key: sum(v[key] for v in devices) for key in
                 ('read_calls', 'write_calls', 'read_bytes', 'write_bytes', 'bounce_bytes')}, devices=devices)


def delta(after, before):
    result = {key: after[key] - before[key] for key in
              ('read_calls', 'write_calls', 'read_bytes', 'write_bytes', 'bounce_bytes')}
    if 'devices' in after:
        result['devices'] = [dict(device_idx=d, **delta(a, b))
                             for d, (a, b) in enumerate(zip(after['devices'], before['devices']))]
    return result


def trial(client, trace, args, segment, shuffled, capacities):
    client.write_call_us = []
    lengths, _ = geometry(trace, segment, args.mode)
    layers = {}
    for request, layer in lengths:
        layers.setdefault(request, []).append(layer)
    # A store has no request dimension. Give each request its own VM/extent;
    # dense layer IDs avoid huge tables for sparsely numbered external layers.
    layer_ids = {req: {layer: i for i, layer in enumerate(sorted(ls))} for req, ls in layers.items()}
    stores, loaded = {}, {}
    mapping = hashlib.sha256()
    records = []
    written_rows = 0
    append_ns = sync_ns = release_ns = 0
    start_stats = native_stats(client.pool)
    initial = time.perf_counter_ns()

    def store_for(req):
        if req not in stores:
            vm = VirtualMedia(client, unit_size=PAGE, capacity_per_device=(dict(enumerate(capacities[req])) if isinstance(capacities[req], list) else capacities[req]),
                              strategy=(LayerPlacement(client.pool.device_count, sorted(layers[req]), req, lengths, args.placement, args.stripe_bytes // PAGE)
                                        if args.placement != 'hash' else
                                        OriginalLayerHash(client.pool.device_count, sorted(layers[req]))),
                              sp_bytes=segment, sp_bytes_per_device={}, num_workers=args.workers,
                              read_merge=not args.no_merge, max_read_bytes=args.max_read_bytes)
            try:
                stores[req] = SparseKVStore(vm, num_layers=len(layer_ids[req]),
                    max_tokens=max(n for (q, _), n in lengths.items() if q == req), max_topk=args.max_topk)
            except BaseException:
                vm.close()
                raise
        return stores[req]

    def append(req, layer, end):
        nonlocal written_rows, append_ns
        key = (req, layer)
        start = loaded.get(key, 0)
        if start == end:
            return
        store = store_for(req)
        dense_layer = layer_ids[req][layer]
        tokens = list(range(start, end))
        if shuffled:
            seed = hashlib.sha256(f'{args.seed}:{req}:{layer}:{start}:{end}'.encode()).digest()
            random.Random(seed).shuffle(tokens)
        begin = time.perf_counter_ns()
        progress_at = time.monotonic()
        print(f"  prepare request={req} layer={layer} rows={end-start}", flush=True)
        # Same one-token KVBlockRef interface in both layouts. Changing insertion
        # order changes real slot-table offsets, not just I/O request order.
        for completed, token in enumerate(tokens, 1):
            data = pattern(trace.bases[key] + token, args.seed)
            store.offload([KVBlockRef(dense_layer, token, token, 1, data)])
            if completed % 1024 == 0 and time.monotonic() - progress_at >= 5:
                elapsed = (time.perf_counter_ns() - begin) / 1e9
                rate = completed / elapsed
                print(f"    appended={completed}/{len(tokens)} slots "
                      f"rate={rate * PAGE / (1 << 20):.2f} MiB/s "
                      f"eta={(len(tokens)-completed)/rate:.0f}s (includes buffered rows)", flush=True)
                progress_at = time.monotonic()
        append_ns += time.perf_counter_ns() - begin
        for token in range(start, end):
            offset, _ = store.slot_table.locate(dense_layer, token)
            device = store.media.locate((dense_layer, token))
            mapping.update(json.dumps([req, layer, token, device, offset], separators=(',', ':')).encode())
        print(f"  prepared request={req} layer={layer} rows={end-start}", flush=True)
        loaded[key] = end
        written_rows += end - start

    def flush(req):
        nonlocal sync_ns
        begin = time.perf_counter_ns()
        print(f"  flush/sync request={req}", flush=True)
        stores[req].flush()
        client.pool.sync()
        sync_ns += time.perf_counter_ns() - begin

    try:
        if args.mode == 'snapshot':
            for req, layer in sorted(lengths):
                append(req, layer, lengths[(req, layer)])
            for req in sorted(stores):
                flush(req)
            client.check_watch('after_snapshot_sync')
        print(f'  replay batches={len(trace.records)} workers={args.workers} merge={not args.no_merge}', flush=True)
        replay_progress = time.monotonic()
        last = {r['request_id']: i for i, r in enumerate(trace.records)}
        for i, r in enumerate(trace.records):
            req, layer = r['request_id'], r['layer_id']
            if args.mode == 'online':
                append(req, layer, r['context_length'])
                # Force all requested tokens to SSD. Report padding costs rather
                # than allowing a DRAM hit to masquerade as a faster SSD read.
                flush(req)
            store = stores[req]
            dense_layer = layer_ids[req][layer]
            indices = r['topk_token_indices']
            outs = [memoryview(bytearray(PAGE)) for _ in indices]
            before = native_stats(client.pool)
            begin = time.perf_counter_ns()
            plan = store.plan(dense_layer, indices)
            plan_ns = time.perf_counter_ns() - begin
            entries = plan.entries()
            if any(entry[1] != FLAG_DISK for entry in entries):
                raise RuntimeError('expected disk-only descriptors after flush')
            for token, entry in zip(indices, entries):
                off, _ = store.slot_table.locate(dense_layer, token)
                device = store.media.locate((dense_layer, token))
                if entry[0] != device or entry[2] != store.media.extent_base(device) + off:
                    raise RuntimeError('plan address mismatch')
            begin = time.perf_counter_ns()
            store.fetch(dense_layer, indices, outs)
            fetch_ns = time.perf_counter_ns() - begin
            stats = delta(native_stats(client.pool), before)
            if stats['read_bytes'] < len(set(indices)) * PAGE or stats['write_bytes']:
                raise RuntimeError('unexpected disk-only fetch statistics')
            begin = time.perf_counter_ns()
            if not args.no_verify:
                for token, data in zip(indices, outs):
                    expected = pattern(trace.bases[(req, layer)] + token, args.seed)
                    if data != expected:
                        raise DataMismatchError(diagnose_mismatch(
                            client, store, dense_layer, r, i, token, data, expected,
                            segment, shuffled, args.workers))
            records.append(dict(record_index=i, request_id=req, layer_id=layer, step_id=r['step_id'],
                                topk=len(indices), unique_topk=len(set(indices)), plan_us=plan_ns / 1e3,
                                fetch_us=fetch_ns / 1e3, verify_us=(time.perf_counter_ns() - begin) / 1e3, io=stats))
            if time.monotonic() - replay_progress >= 5:
                print(f'    replayed={i+1}/{len(trace.records)} batches', flush=True)
                replay_progress = time.monotonic()
            if i == last[req]:
                begin = time.perf_counter_ns()
                for (q, lyr), end in loaded.items():
                    if q == req:
                        store.release(layer_ids[req][lyr], list(range(end)))
                if any(store.media.stats()['buffered_units']):
                    raise RuntimeError('buffer still occupied at release')
                caps = capacities[req] if isinstance(capacities[req], list) else [capacities[req]]
                expected_free = [size // segment for size in caps]
                if store.media.stats()['free_segments'] != expected_free:
                    raise RuntimeError('released segments were not reclaimed')
                store.close()
                del stores[req]
                release_ns += time.perf_counter_ns() - begin
        io = delta(native_stats(client.pool), start_stats)
        if client.live:
            raise RuntimeError('native extents leaked')
        seconds = sum(r['fetch_us'] for r in records) / 1e6
        read_bytes = sum(r['io']['read_bytes'] for r in records)
        useful = sum(r['unique_topk'] * KV_BYTES for r in records)
        return dict(segment_bytes=segment, layout='shuffled' if shuffled else 'ordered',
                    mapping_sha256=mapping.hexdigest(), batches=records, io=io,
                    written_rows=written_rows, write_amplification_vs_slots=io['write_bytes'] / (written_rows * PAGE),
                    read_amplification_vs_unique_kv=read_bytes / useful,
                    write_call_us=percentiles(client.write_call_us) if client.write_call_us else {},
                    write_call_count=len(client.write_call_us), write_call_total_ms=sum(client.write_call_us) / 1e3,
                    append_ms=append_ns / 1e6, flush_sync_ms=sync_ns / 1e6, release_ms=release_ns / 1e6,
                    wall_ms=(time.perf_counter_ns() - initial) / 1e6,
                    active_fetch_seconds=seconds, fetch_us=percentiles([r['fetch_us'] for r in records]),
                    plan_us=percentiles([r['plan_us'] for r in records]),
                    effective_unique_kv_MiB_s=useful / (1 << 20) / seconds)
    finally:
        for store in stores.values():
            store.close()


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--trace', type=Path, required=True)
    target = p.add_mutually_exclusive_group(required=True)
    target.add_argument('--file', type=Path, help='NEW regular file')
    target.add_argument('--devices-config', type=Path, help='JSON array of eight explicit device windows (or eight new test files)')
    target.add_argument('--device', type=Path, help='block device with an authorized overwrite window')
    p.add_argument('--window-offset', type=size_arg)
    p.add_argument('--window-bytes', type=size_arg)
    p.add_argument('--allow-device-write', action='store_true')
    p.add_argument('--dry-run', action='store_true', help='print capacity/window plan without opening the target or writing files')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--mode', choices=['snapshot', 'online'], default='snapshot')
    p.add_argument('--placement', choices=['hash', 'range', 'stripe'], default='hash')
    p.add_argument('--stripe-bytes', type=size_arg, default=1 << 20,
                   help='stripe placement only: logical slot bytes per stripe, independent of write segment; default 1 MiB')
    p.add_argument('--layouts', nargs='+', choices=['ordered', 'shuffled'], default=['ordered', 'shuffled'])
    p.add_argument('--experiment', choices=['write-layout', 'layout-concurrency'], default='write-layout')
    p.add_argument('--worker-sweep', type=int, nargs='+', help='layout-concurrency only; default: 1 4 8 16')
    p.add_argument('--segments', type=size_arg, nargs='+', default=[1 << 20])
    p.add_argument('--workers', type=int, default=4)
    p.add_argument('--max-topk', type=int, default=2048)
    p.add_argument('--max-read-bytes', type=size_arg, default=1 << 20)
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--seed', type=int, default=2026)
    p.add_argument('--no-merge', action='store_true')
    p.add_argument('--no-verify', action='store_true')
    p.add_argument('--watch-device-offset', nargs=2, type=int, metavar=('DEVICE_IDX', 'BYTE_OFFSET'),
                   help='diagnostic snapshot only: read back one physical 4096-byte page after writes and snapshot sync')
    return p


def run(args):
    if args.placement != 'hash' and (args.mode != 'snapshot' or not args.devices_config):
        raise ValueError('range/stripe placement requires snapshot and devices-config')
    if args.stripe_bytes < PAGE or args.stripe_bytes % PAGE:
        raise ValueError('stripe-bytes must be a positive multiple of 4096')
    if min(args.workers, args.max_topk, args.repeats) < 1 or not 0 <= args.seed < 1 << 64:
        raise ValueError('invalid workers, max-topk, repeats or seed')
    layout_experiment = args.experiment == 'layout-concurrency'
    if layout_experiment:
        if args.mode != 'snapshot' or len(args.segments) != 1 or args.segments[0] <= PAGE:
            raise ValueError('layout-concurrency requires snapshot and exactly one preparation segment > 4096')
        args = copy.copy(args)
        args.no_merge = True
        worker_values = sorted(set(args.worker_sweep if args.worker_sweep is not None else [1, 4, 8, 16]))
        if not worker_values or min(worker_values) < 1:
            raise ValueError('worker-sweep must contain positive integers')
    else:
        if args.worker_sweep is not None:
            raise ValueError('--worker-sweep requires --experiment layout-concurrency')
        worker_values = [args.workers]
    segments = sorted(set(args.segments if layout_experiment else [PAGE] + args.segments))
    if any(s < PAGE or s % PAGE for s in segments):
        raise ValueError('VirtualMedia segments must be multiples of 4096')
    if args.max_read_bytes < PAGE or args.max_read_bytes % PAGE:
        raise ValueError('max-read-bytes must be a multiple of 4096')
    trace = load_trace(args.trace, args.max_topk)
    lengths, capacities = geometry(trace, max(segments), args.mode)
    per_device_rows = None
    if args.devices_config:
        if args.window_offset is not None or args.window_bytes is not None:
            raise ValueError('window options belong in devices-config')
        windows = load_devices(args.devices_config)
        if args.placement != 'hash':
            capacities, per_device_rows = layer_capacities(max(segments), lengths, 8,
                                                          args.placement, args.stripe_bytes // PAGE)
        else:
            capacities, per_device_rows = hash_capacities(trace, max(segments), args.mode, 8, lengths)
        required = [sum(caps[d] for caps in capacities.values()) for d in range(8)]
    else:
        required = [sum(capacities.values())]
        base, window = 0, required[0]
        if args.device:
            if args.window_offset is None or args.window_bytes is None:
                raise ValueError('device requires explicit --window-offset and --window-bytes')
            base, window = args.window_offset, args.window_bytes
        elif args.window_offset is not None or args.window_bytes is not None:
            raise ValueError('device window options require --device')
        windows = [dict(device_idx=0, target=(args.file or args.device).resolve(),
                        kind='device' if args.device else 'file', window_offset=base, window_bytes=window)]
    block_mode = windows[0]['kind'] == 'device'
    if args.watch_device_offset is not None:
        d, watched = args.watch_device_offset
        if args.mode != 'snapshot' or not 0 <= d < len(windows):
            raise ValueError('watch-device-offset requires snapshot and a valid device index')
        w = windows[d]
        if watched % PAGE or watched < w['window_offset'] or watched + PAGE > w['window_offset'] + w['window_bytes']:
            raise ValueError('watched page must be aligned and inside the configured window')
    if block_mode and not args.dry_run and not args.allow_device_write:
        raise ValueError('device writes require --allow-device-write')
    if not block_mode and args.allow_device_write:
        raise ValueError('--allow-device-write requires block devices')
    protected = {args.trace.resolve(), args.output.resolve()}
    if args.devices_config:
        protected.add(args.devices_config.resolve())
        if args.devices_config.resolve() == args.output.resolve():
            raise ValueError('config and output must differ')
    if args.trace.resolve() == args.output.resolve():
        raise ValueError('trace and output must differ')
    for w, need in zip(windows, required):
        base, window, target = w['window_offset'], w['window_bytes'], w['target']
        if base < 0 or base % PAGE or window < need or window % PAGE:
            raise ValueError(f"device {w['device_idx']}: window must be 4096-byte aligned and at least {need} bytes")
        if base + window > (1 << 63) - 1:
            raise ValueError('window end exceeds signed 64-bit offset range')
        if target in protected:
            raise ValueError('target, trace, config and output must differ')
        if w['kind'] == 'file' and target.exists():
            raise FileExistsError('test data files must be new')
        w['capacity_bytes'] = need
        w['possible_write_range'] = [base, base + need]
    if args.output.exists():
        raise FileExistsError('result path must be new')
    capacity = sum(required)
    target, base, window = windows[0]['target'], windows[0]['window_offset'], windows[0]['window_bytes']
    result = dict(experiment=args.experiment, worker_sweep=worker_values, schema_version=2, status='running', mode=args.mode, path='SparseKVStore/VirtualMedia/native SSD pool',
                  trace_sha256=trace.digest, trace_path=str(args.trace.resolve()),
                  file=str(target) if args.file else None, target=str(target) if len(windows) == 1 else None,
                  target_kind='block_device' if block_mode else 'regular_file',
                  device_count=len(windows), devices=[dict(w, target=str(w['target'])) for w in windows],
                  hash_strategy=dict(name='position_hash', step_idx=17, step_layer=23, prime=2147483647,
                                     layer_key='original model layer_id'),
                  rows_per_request_device=per_device_rows,
                  window_offset=base if len(windows) == 1 else None, window_bytes=window if len(windows) == 1 else None,
                  possible_write_range=windows[0]['possible_write_range'] if len(windows) == 1 else None,
                  capacity_bytes=capacity, request_capacities=capacities, machine=platform.machine(),
                  kernel=platform.release(), python=platform.python_version(), seed=args.seed,
                  workers=None if layout_experiment else args.workers, max_read_bytes=args.max_read_bytes, merge=not args.no_merge,
                  verified=not args.no_verify, segments=segments, repeats=args.repeats,
                  slot_bytes=PAGE, kv_bytes=KV_BYTES, trials=[], comparisons=[],
                  notes=['shuffled is a synthetic insertion-order baseline, not measured production layout',
                         'plan is timed separately; fetch repeats lookup internally; do not add them as pipeline latency',
                         'fetch includes Python scheduling, allocations and native alignment bounce copies',
                         'first fetch includes lazy worker creation; no warmup or cache eviction',
                         'online flushes before each fetch; padding and synchronization costs are reported',
                         'firmware caches, FTL, physical die mapping and model TPOT are not controlled'])
    if layout_experiment:
        result['notes'].append('each trial prepares a fresh snapshot with the same large write segment; preparation is excluded from fetch time; workers is the total host thread count, not per-device queue depth')
    result['watch_device_offset'] = args.watch_device_offset
    result['placement'] = args.placement
    result['layouts'] = sorted(set(args.layouts))
    result['stripe_bytes'] = args.stripe_bytes if args.placement == 'stripe' else None
    if args.placement != 'hash':
        result['hash_strategy'] = None
        result['notes'].append('placement restarts at device 0 for each request/layer; range uses balanced contiguous token intervals from snapshot max context; stripe uses token // stripe_rows modulo device count; stripe size and write segment size are independent')
    if args.watch_device_offset is not None:
        result['notes'].append('diagnostic run: watch reads affect preparation I/O statistics and cache state; do not use for performance comparisons')
    if args.dry_run:
        result['status'] = 'planned'
        result['notes'].append('target existence/type/size/exclusivity and O_DIRECT support have NOT been checked')
        print(json.dumps(result, indent=2))
        return result
    if block_mode:
        identities = []
        for w in windows:
            info = w['target'].stat()
            if not stat.S_ISBLK(info.st_mode):
                raise ValueError('--device must be a block device')
            identities.append(info.st_rdev)
        if len(set(identities)) != len(identities):
            raise ValueError('duplicate block device identity')
    with args.output.open('x') as output:
        pool = None
        try:
            if not block_mode:
                for w in windows:
                    fd = os.open(w['target'], os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
                    try:
                        os.posix_fallocate(fd, 0, w['window_offset'] + w['window_bytes'])
                        os.fsync(fd)
                    finally:
                        os.close(fd)
            previous_allow = os.environ.get('UMM_ALLOW_BLOCK_DEVICE')
            try:
                if block_mode:
                    os.environ['UMM_ALLOW_BLOCK_DEVICE'] = '1'
                if len(windows) == 1:
                    pool = DirectPool(target, base, window)
                else:
                    pool = DirectPool(target, base, window, devices=[
                        (w['target'], w['window_offset'], w['window_bytes']) for w in windows])
            finally:
                if block_mode:
                    if previous_allow is None:
                        os.environ.pop('UMM_ALLOW_BLOCK_DEVICE', None)
                    else:
                        os.environ['UMM_ALLOW_BLOCK_DEVICE'] = previous_allow
            client = PoolClient(pool, args.watch_device_offset)
            result['write_watch'] = client.watch_events
            result['library_sha256'] = hashlib.sha256(Path(pool.library_path).read_bytes()).hexdigest()
            cases = [(s, shuffled, workers) for s in segments for workers in worker_values for shuffled in (False, True)
                     if ('shuffled' if shuffled else 'ordered') in args.layouts]
            for repeat in range(args.repeats):
                for segment, shuffled, workers in (cases if repeat % 2 == 0 else list(reversed(cases))):
                    result['active_trial'] = dict(repeat=repeat, segment_bytes=segment,
                                                  layout='shuffled' if shuffled else 'ordered', workers=workers)
                    client.watch_expected = None
                    client.watch_context = result['active_trial'].copy()
                    print(f'repeat={repeat} workers={workers} segment={segment} layout={"shuffled" if shuffled else "ordered"}', flush=True)
                    trial_args = copy.copy(args)
                    trial_args.workers = workers
                    item = trial(client, trace, trial_args, segment, shuffled, capacities)
                    item['workers'] = workers
                    item['placement'] = args.placement
                    item['repeat'] = repeat
                    result['trials'].append(item)
                    output.seek(0)
                    json.dump(result, output, indent=2)
                    output.truncate()
                    output.flush()
            def compare(base_layout, base_seg, base_workers, layout, seg, workers):
                a = [t['active_fetch_seconds'] for t in result['trials'] if
                     (t['layout'], t['segment_bytes'], t['workers']) == (base_layout, base_seg, base_workers)]
                b = [t['active_fetch_seconds'] for t in result['trials'] if
                     (t['layout'], t['segment_bytes'], t['workers']) == (layout, seg, workers)]
                return dict(baseline=[base_layout, base_seg], candidate=[layout, seg],
                            baseline_workers=base_workers, candidate_workers=workers,
                            median_fetch_speedup=statistics.median(a) / statistics.median(b),
                            paired_speedups=[x / y for x, y in zip(a, b)])
            for seg in segments:
                for workers in worker_values:
                    if {'ordered', 'shuffled'}.issubset(args.layouts):
                        result['comparisons'].append(compare('shuffled', seg, workers, 'ordered', seg, workers))
                    if layout_experiment:
                        if workers != worker_values[0]:
                            for layout in sorted(set(args.layouts)):
                                result['comparisons'].append(compare(layout, seg, worker_values[0], layout, seg, workers))
                    elif seg != PAGE:
                        for layout in sorted(set(args.layouts)):
                            result['comparisons'].append(compare(layout, PAGE, workers, layout, seg, workers))
                        if {'ordered', 'shuffled'}.issubset(args.layouts):
                            result['comparisons'].append(compare('shuffled', PAGE, workers, 'ordered', seg, workers))
            result['status'] = 'complete'
            result.pop('active_trial', None)
        except BaseException as exc:
            result['status'] = 'failed'
            result['error'] = str(exc)
            if isinstance(exc, DataMismatchError):
                result['failure'] = exc.details
                device = exc.details['device_idx']
                result['failure']['device_path'] = str(windows[device]['target'])
            raise
        finally:
            if pool:
                pool.close()
            output.seek(0)
            json.dump(result, output, indent=2)
            output.truncate()
    return result


def main():
    run(parser().parse_args())


if __name__ == '__main__':
    main()
