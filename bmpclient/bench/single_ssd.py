"""Single-SSD snapshot trace replay through libumm's real SSD pool/backend.

4 KiB slots, 1152 B useful KV per row. This is a data-plane microbenchmark,
not a model TPOT benchmark. Cases share the same file/window and row mapping.
"""
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import stat
import statistics
import struct
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass

PAGE = 4096
KV_BYTES = 1152


def size_arg(value):
    value = str(value).strip().upper()
    for suffix, scale in (("GIB", 1 << 30), ("MIB", 1 << 20), ("KIB", 1 << 10),
                          ("G", 1 << 30), ("M", 1 << 20), ("K", 1 << 10)):
        if value.endswith(suffix):
            return int(value[:-len(suffix)]) * scale
    return int(value, 0)


def integer(value, name, minimum=0):
    if type(value) is not int or value < minimum:
        raise ValueError(f"{name} must be an integer >= {minimum}")
    return value


@dataclass
class Trace:
    records: list
    bases: dict
    rows: int
    digest: str


def load_trace(path, max_topk):
    records, lengths = [], {}
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for line_no, raw in enumerate(stream, 1):
            digest.update(raw)
            if not raw.strip():
                continue
            try:
                r = json.loads(raw)
                if not isinstance(r, dict):
                    raise ValueError("record must be an object")
                if not isinstance(r.get("request_id"), str) or not r["request_id"]:
                    raise ValueError("request_id must be a nonempty string")
                integer(r["step_id"], "step_id")
                integer(r["layer_id"], "layer_id")
                ctx = integer(r["context_length"], "context_length", 1)
                indices = r["topk_token_indices"]
                if not isinstance(indices, list) or not 1 <= len(indices) <= max_topk:
                    raise ValueError(f"topk must have 1..{max_topk} entries")
                for token in indices:
                    if integer(token, "token index") >= ctx:
                        raise ValueError("token index must be < context_length")
                key = (r["request_id"], r["layer_id"])
                lengths[key] = max(lengths.get(key, 0), ctx)
                records.append(r)
            except (ValueError, KeyError, TypeError) as exc:
                raise ValueError(f"trace line {line_no}: {exc}") from exc
    if not records:
        raise ValueError("trace is empty")
    bases, rows = {}, 0
    # Stable request/layer layout, independent of read order and trial order.
    for key in sorted(lengths):
        bases[key] = rows
        rows += lengths[key]
    return Trace(records, bases, rows, digest.hexdigest())


def plan_rows(indices, base, merge, max_read):
    """Return unique rows, their output slots, and contiguous bounded runs.

    Duplicate top-k entries share a physical read; logical positions retained
    in slots. No over-read/gap coalescing, identical policy for every write case.
    """
    rows = sorted(set(base + t for t in indices))
    lookup = {row: i for i, row in enumerate(rows)}
    slots = [lookup[base + t] for t in indices]
    runs = []
    max_rows = max_read // PAGE
    for pos, row in enumerate(rows):
        if merge and runs and row == runs[-1][0] + runs[-1][2] and runs[-1][2] < max_rows:
            start, dst, count = runs[-1]
            runs[-1] = (start, dst, count + 1)
        else:
            runs.append((row, pos, 1))
    return rows, slots, runs


def pattern(row, seed):
    # Include random padding so compression/dedup doesn't favor one layout.
    return hashlib.shake_256(struct.pack("<QQ", row, seed)).digest(PAGE)


class AlignedBuffer:
    _libc = ctypes.CDLL(None)
    _libc.posix_memalign.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_size_t, ctypes.c_size_t]
    _libc.posix_memalign.restype = ctypes.c_int
    _libc.free.argtypes = [ctypes.c_void_p]

    def __init__(self, size, alignment):
        self.address = ctypes.c_void_p()
        rc = self._libc.posix_memalign(ctypes.byref(self.address), alignment, size)
        if rc:
            raise MemoryError(f"posix_memalign: {rc}")
        self.size = size

    def close(self):
        if self.address.value:
            self._libc.free(self.address)
            self.address.value = None


class IOStats(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in
                ("read_calls", "write_calls", "read_bytes", "write_bytes", "bounce_bytes",
                 "alignment", "window_base")]

    def dictionary(self):
        return {name: getattr(self, name) for name, _ in self._fields_}


class DirectPool:
    def __init__(self, path, window_base, capacity, devices=None):
        from bmpclient.umm_client import find_libumm_so
        self.library_path = find_libumm_so()
        self.lib = ctypes.CDLL(self.library_path)
        declarations = {
            "ssd_pool_create": ([], ctypes.c_void_p),
            "ssd_pool_destroy": ([ctypes.c_void_p], None),
            "ssd_pool_add_device": ([ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint64], ctypes.c_int),
            "ssd_pool_alloc_on_device": ([ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64,
                                           ctypes.POINTER(ctypes.c_uint64)], ctypes.c_int),
            "ssd_pool_pread": ([ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_void_p], ctypes.c_int),
            "ssd_pool_pwrite": ([ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_void_p], ctypes.c_int),
            "ssd_pool_sync": ([ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64], ctypes.c_int),
            "ssd_pool_direct_stats": ([ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(IOStats), ctypes.c_int], ctypes.c_int),
        }
        for name, (args, result) in declarations.items():
            fn = getattr(self.lib, name)
            fn.argtypes, fn.restype = args, result
        self.pool = self.lib.ssd_pool_create()
        if not self.pool:
            raise MemoryError("ssd_pool_create")
        self.reservations = []
        self.device_count = 0
        try:
            total = 0
            for i, (device_path, base, size) in enumerate(devices or [(path, window_base, capacity)]):
                self.check(self.lib.ssd_pool_add_device(self.pool, f"direct:{base}:{device_path}".encode(), size))
                offset = ctypes.c_uint64()
                self.check(self.lib.ssd_pool_alloc_on_device(self.pool, i, size, ctypes.byref(offset)))
                if offset.value != total:
                    raise RuntimeError("unexpected allocation base")
                self.reservations.append((offset.value, size))
                self.device_count += 1
                total += size
            self.capacity = total
            self.alignment = self.stats()["alignment"]
        except BaseException:
            self.close()
            raise

    @staticmethod
    def check(rc):
        if rc:
            raise RuntimeError(f"UMM direct backend error {rc}; see native log (no buffered fallback)")

    def stats(self, reset=False, device_idx=0):
        out = IOStats()
        self.check(self.lib.ssd_pool_direct_stats(self.pool, device_idx, ctypes.byref(out), reset))
        return out.dictionary()

    def write(self, off, size, ptr):
        self.check(self.lib.ssd_pool_pwrite(self.pool, off, size, ptr))

    def read(self, off, size, ptr):
        self.check(self.lib.ssd_pool_pread(self.pool, off, size, ptr))

    def sync(self):
        for off, size in self.reservations:
            self.check(self.lib.ssd_pool_sync(self.pool, off, size))

    def close(self):
        if self.pool:
            self.lib.ssd_pool_destroy(self.pool)
            self.pool = None


def percentiles(values):
    ordered = sorted(values)
    return {name: ordered[min(len(ordered) - 1, math.ceil(q * len(ordered)) - 1)]
            for name, q in (("p50", .5), ("p95", .95), ("p99", .99))}


def prepare(pool, dataset_bytes, write_size, generation_chunk, seed):
    """Same data/host generation chunk for all cases; time generation separately."""
    buffer = AlignedBuffer(generation_chunk, pool.alignment)
    io_ns = 0
    begin = time.perf_counter_ns()
    pool.stats(reset=True)
    try:
        for start in range(0, dataset_bytes, generation_chunk):
            length = min(generation_chunk, dataset_bytes - start)
            generated = b"".join(pattern(row, seed) for row in range(start // PAGE, (start + length) // PAGE))
            for local in range(0, length, write_size):
                size = min(write_size, length - local)
                ctypes.memmove(buffer.address, generated[local:local + size], size)
                t = time.perf_counter_ns()
                pool.write(start + local, size, buffer.address)
                io_ns += time.perf_counter_ns() - t
        sync_start = time.perf_counter_ns()
        pool.sync()
        sync_ns = time.perf_counter_ns() - sync_start
    finally:
        buffer.close()
    stats = pool.stats()
    if stats["write_bytes"] != dataset_bytes or stats["bounce_bytes"]:
        raise RuntimeError(f"unexpected write statistics: {stats}")
    return {"wall_ms": (time.perf_counter_ns() - begin) / 1e6,
            "io_call_ms": io_ns / 1e6, "sync_ms": sync_ns / 1e6, "io": stats}


def replay(pool, trace, workers, merge, max_read, max_topk, seed, verify, warmup):
    local = threading.local()
    buffers = []
    buffer_lock = threading.Lock()
    output = AlignedBuffer(max_topk * PAGE, pool.alignment)
    try:
        ordered_output = AlignedBuffer(max_topk * PAGE, pool.alignment)
    except BaseException:
        output.close()
        raise
    def initialize():
        local.buffer = AlignedBuffer(max_read, pool.alignment)
        with buffer_lock:
            buffers.append(local.buffer)
    def read_run(run):
        row, dst, count = run
        length = count * PAGE
        begin = time.perf_counter_ns()
        pool.read(row * PAGE, length, local.buffer.address)
        io_ns = time.perf_counter_ns() - begin
        begin = time.perf_counter_ns()
        ctypes.memmove(output.address.value + dst * PAGE, local.buffer.address, length)
        return io_ns, time.perf_counter_ns() - begin
    records = []
    try:
        with ThreadPoolExecutor(max_workers=workers, initializer=initialize) as executor:
            # Prestart all threads/buffers before measurements (no lazy allocations).
            barrier = threading.Barrier(workers)
            futures = [executor.submit(barrier.wait, 30) for _ in range(workers)]
            for future in futures:
                future.result()
            pool.stats(reset=True)
            sequence = trace.records[:warmup] + trace.records
            for n, record in enumerate(sequence):
                if n == warmup:
                    pool.stats(reset=True)
                begin = time.perf_counter_ns()
                base = trace.bases[(record["request_id"], record["layer_id"])]
                rows, slots, runs = plan_rows(record["topk_token_indices"], base, merge, max_read)
                planned = time.perf_counter_ns()
                futures = [executor.submit(read_run, run) for run in runs]
                times = [future.result() for future in futures]
                io_finished = time.perf_counter_ns()
                # Restore original top-k order, including duplicates.
                for pos, slot in enumerate(slots):
                    ctypes.memmove(ordered_output.address.value + pos * PAGE,
                                   output.address.value + slot * PAGE, PAGE)
                finished = time.perf_counter_ns()
                verify_ns = 0
                if verify:
                    t = time.perf_counter_ns()
                    for pos, token in enumerate(record["topk_token_indices"]):
                        row = base + token
                        actual = ctypes.string_at(ordered_output.address.value + pos * PAGE, PAGE)
                        if actual != pattern(row, seed):
                            raise RuntimeError(f"data mismatch request={record['request_id']} layer={record['layer_id']} row={row}")
                    verify_ns = time.perf_counter_ns() - t
                if n >= warmup:
                    records.append({"record_index": n - warmup,
                        "request_id": record["request_id"], "step_id": record["step_id"],
                        "layer_id": record["layer_id"], "topk": len(slots), "unique_rows": len(rows),
                        "planned_reads": len(runs), "read_bytes": len(rows) * PAGE,
                        "logical_kv_bytes": len(slots) * KV_BYTES, "unique_kv_bytes": len(rows) * KV_BYTES,
                        "plan_us": (planned - begin) / 1e3, "io_batch_us": (io_finished - planned) / 1e3,
                        "gather_us": (finished - io_finished) / 1e3,
                        "batch_us": (finished - begin) / 1e3,
                        "sum_io_call_us": sum(t[0] for t in times) / 1e3,
                        "sum_copy_us": sum(t[1] for t in times) / 1e3,
                        "verify_us": verify_ns / 1e3})
        stats = pool.stats()
        expected_bytes = sum(r["read_bytes"] for r in records)
        if stats["read_bytes"] != expected_bytes or stats["bounce_bytes"]:
            raise RuntimeError(f"unexpected read statistics: {stats}, expected={expected_bytes}")
        seconds = sum(r["batch_us"] for r in records) / 1e6
        unique_bytes = sum(r["unique_kv_bytes"] for r in records)
        return {"batch_us": percentiles([r["batch_us"] for r in records]),
                "io_batch_us": percentiles([r["io_batch_us"] for r in records]),
                "active_batch_seconds": seconds,
                "effective_unique_kv_MiB_s": unique_bytes / (1 << 20) / seconds,
                "host_read_MiB_s": expected_bytes / (1 << 20) / seconds,
                "read_amplification": expected_bytes / unique_bytes,
                "io": stats, "batches": records}
    finally:
        for buffer in buffers:
            buffer.close()
        output.close()
        ordered_output.close()


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--trace", type=Path, required=True, help="JSONL real or synthetic top-k records")
    target = p.add_mutually_exclusive_group(required=True)
    target.add_argument("--file", type=Path, help="new benchmark file on the target SSD filesystem")
    target.add_argument("--device", type=Path, help="exclusive disposable block device/partition")
    p.add_argument("--reuse-file", action="store_true", help="explicitly overwrite the existing file's test region")
    p.add_argument("--allow-device-write", action="store_true", help="authorize writes to the specified block-device window")
    p.add_argument("--window-offset", type=size_arg, default=None)
    p.add_argument("--window-bytes", type=size_arg, default=None)
    p.add_argument("--segments", type=size_arg, nargs="+", default=[1 << 20, 4 << 20])
    p.add_argument("--workers", type=int, default=4, help="maximum concurrent synchronous reads, not hardware queue depth")
    p.add_argument("--max-read-bytes", type=size_arg, default=1 << 20)
    p.add_argument("--max-topk", type=int, default=2048)
    p.add_argument("--repeats", type=int, default=3, help="alternate case order each repetition")
    p.add_argument("--warmup-batches", type=int, default=0)
    p.add_argument("--no-merge", action="store_true")
    p.add_argument("--no-verify", action="store_true", help="skip full per-batch verification outside timed region")
    p.add_argument("--seed", type=int, default=2026)
    p.add_argument("--output", type=Path, required=True, help="new JSON result file")
    return p


def run(args):
    for name in ("workers", "max_topk", "repeats"):
        integer(getattr(args, name), name, 1)
    integer(args.warmup_batches, "warmup_batches")
    integer(args.seed, "seed")
    if args.seed >= 1 << 64:
        raise ValueError("seed must fit u64")
    sizes = sorted(set([PAGE] + args.segments))
    if any(s < PAGE or s % PAGE for s in sizes) or any(max(sizes) % s for s in sizes):
        raise ValueError("segment sizes must be multiples of 4096 and divide the largest segment")
    if args.max_read_bytes < PAGE or args.max_read_bytes % PAGE:
        raise ValueError("max-read-bytes must be a positive multiple of 4096")
    trace = load_trace(args.trace, args.max_topk)
    if args.warmup_batches > len(trace.records):
        raise ValueError("warmup-batches exceeds trace length")
    dataset = ((trace.rows * PAGE + max(sizes) - 1) // max(sizes)) * max(sizes)
    base = args.window_offset if args.window_offset is not None else 0
    capacity = args.window_bytes if args.window_bytes is not None else dataset
    if base < 0 or base % PAGE or capacity < dataset or capacity % PAGE:
        raise ValueError("window must be 4 KiB aligned and hold the complete padded dataset")
    target = (args.file or args.device).absolute()
    if args.output.exists():
        raise FileExistsError(f"result file already exists: {args.output}")
    if target.exists() and os.path.samefile(target, args.trace):
        raise ValueError("target is an alias of trace file")
    if target.resolve() in (args.output.resolve(), args.trace.resolve()) or args.output.resolve() == args.trace.resolve():
        raise ValueError("trace, data target and output must be different paths")
    if args.device:
        if not args.allow_device_write or args.window_offset is None or args.window_bytes is None:
            raise ValueError("device requires --allow-device-write, --window-offset and --window-bytes")
        if not stat.S_ISBLK(target.stat().st_mode):
            raise ValueError("--device must be a block device")
        os.environ["UMM_ALLOW_BLOCK_DEVICE"] = "1"
    else:
        flags = os.O_RDWR | (0 if args.reuse_file else os.O_CREAT | os.O_EXCL)
        fd = os.open(target, flags, 0o600)
        try:
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                raise ValueError("--file must be a regular file")
            if args.reuse_file:
                if os.fstat(fd).st_size < base + capacity:
                    raise ValueError("existing file too small; refusing to resize it")
            else:
                os.posix_fallocate(fd, 0, base + capacity)
            os.fsync(fd)
        finally:
            os.close(fd)
    # Reserve output before any destructive benchmark writes; never overwrite results.
    with args.output.open("x") as output:
        pool = DirectPool(target, base, capacity)
        try:
            result = {"schema_version": 1, "status": "running", "mode": "single_ssd_snapshot_direct_io",
                "machine": platform.machine(), "kernel": platform.release(), "python": platform.python_version(),
                "os_release": Path("/etc/os-release").read_text() if Path("/etc/os-release").exists() else "",
                "trace_sha256": trace.digest, "trace_path": str(args.trace.resolve()),
                "target": str(target), "target_kind": "block_device" if args.device else "regular_file",
                "window_offset": base, "window_bytes": capacity, "dataset_bytes": dataset,
                "kv_rows": trace.rows, "slot_bytes": PAGE, "kv_bytes": KV_BYTES,
                "generation_chunk_bytes": max(sizes), "write_sizes": sizes, "repeats": args.repeats,
                "workers": args.workers, "merge": not args.no_merge, "max_read_bytes": args.max_read_bytes,
                "verified": not args.no_verify, "warmup_batches": args.warmup_batches,
                "seed": args.seed, "library_path": pool.library_path,
                "library_sha256": hashlib.sha256(Path(pool.library_path).read_bytes()).hexdigest(),
                "notes": ["O_DIRECT requested; no application buffered fallback or mmap",
                          "All rows up to max context per request/layer are preloaded (snapshot, not online decode)",
                          "Same row-to-file-offset mapping and padded write bytes across cases; file offsets are not physical NAND addresses",
                          "Verification is outside timed batches but creates inter-batch idle time",
                          "Reported bandwidth divides by summed active batch times, not total experiment wall time",
                          "SSD/controller caches, FTL state and filesystem extent mapping are not controlled"],
                "trials": [], "comparisons": []}
            json.dump(result, output, ensure_ascii=False, indent=2)
            output.flush()
            for repeat in range(args.repeats):
                for write_size in (sizes if repeat % 2 == 0 else list(reversed(sizes))):
                    name = "small_write" if write_size == PAGE else f"aggregate_{write_size}"
                    print(f"repeat={repeat} case={name} dataset={dataset} bytes", flush=True)
                    trial = {"repeat": repeat, "case": name, "write_size": write_size}
                    trial["prepare"] = prepare(pool, dataset, write_size, max(sizes), args.seed)
                    trial["read"] = replay(pool, trace, args.workers, not args.no_merge,
                        args.max_read_bytes, args.max_topk, args.seed, not args.no_verify, args.warmup_batches)
                    result["trials"].append(trial)
                    output.seek(0)
                    json.dump(result, output, ensure_ascii=False, indent=2)
                    output.truncate()
                    output.flush()
                    print(f"  p50={trial['read']['batch_us']['p50']:.1f} us "
                          f"KV={trial['read']['effective_unique_kv_MiB_s']:.2f} MiB/s", flush=True)
            baseline = [t["read"]["active_batch_seconds"] for t in result["trials"] if t["write_size"] == PAGE]
            for write_size in sizes[1:]:
                candidate = [t["read"]["active_batch_seconds"] for t in result["trials"] if t["write_size"] == write_size]
                result["comparisons"].append({"write_size": write_size,
                    "median_active_batch_speedup_vs_small_write": statistics.median(baseline) / statistics.median(candidate),
                    "paired_repeat_speedups": [a / b for a, b in zip(baseline, candidate)]})
            result["status"] = "complete"
        except BaseException as exc:
            if "result" in locals():
                result["status"] = "failed"
                result["error"] = str(exc)
            raise
        finally:
            if "result" in locals():
                output.seek(0)
                json.dump(result, output, ensure_ascii=False, indent=2)
                output.truncate()
                output.flush()
            pool.close()
    return result


def main():
    args = parser().parse_args()
    run(args)


if __name__ == "__main__":
    main()
