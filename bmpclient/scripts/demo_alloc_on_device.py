"""Real libumm integration: direct, shared-pool RPC and remote RPC.

Run after `make -C umm`: python3 bmpclient/scripts/demo_alloc_on_device.py
Uses temporary files only; never opens a physical block device.
"""
import ctypes
import os
from pathlib import Path
import socket
import subprocess
import struct
import threading
import sys
import tempfile
import time
from contextlib import contextmanager

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
os.environ.setdefault("UMM_BUILD_DIR", str(ROOT / "umm/build"))
from bmpclient.umm_client import UMMLib, UMMConfig, UMM_TIER_SSD, UMM_NODE_UNKNOWN
from bmpclient.virtual_media import VirtualMedia
from bmpclient.sparse_kv import SparseKVStore, KVBlockRef

CAP = 1024 * 1024
PAGE = 4096
OWNER = 7


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_port(port, proc):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited: {proc.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError(f"server port {port}")


@contextmanager
def servers(tmp, paths):
    meta_port, mem_port = free_port(), free_port()
    while mem_port == meta_port:
        mem_port = free_port()
    config = tmp / "umms.yaml"
    config.write_text(
        f'my_node_id: {OWNER}\nlisten_addr: "127.0.0.1"\nlisten_port: {mem_port}\n'
        f'memory_size: {CAP}\nbase_gpa: 0\n'
        'ssd_devices: "' + ','.join(f'{p}:{CAP}' for p in paths) + '"\n'
    )
    procs = []
    with (tmp / "servers.log").open("w") as log:
        try:
            for cmd, port in [
                ([str(ROOT / "umm/bin/ummd"), "-p", str(meta_port), "-b", "127.0.0.1"], meta_port),
                ([str(ROOT / "umm/bin/umms"), "-c", str(config)], mem_port),
            ]:
                proc = subprocess.Popen(cmd, stdout=log, stderr=log)
                procs.append(proc)
                wait_port(port, proc)
            yield meta_port, mem_port
        except BaseException:
            log.flush()
            print((tmp / "servers.log").read_text()[-8000:], file=sys.stderr)
            raise
        finally:
            for proc in reversed(procs):
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


def check(lib, paths, owner):
    topo = lib.get_topology()
    devices = [topo.resources[i] for i in range(topo.num_resources)
               if topo.resources[i].tier == UMM_TIER_SSD]
    assert len(devices) == len(paths), (len(devices), len(paths))
    assert topo.node_id == owner
    for i, dev in enumerate(devices):
        assert Path(dev.device_path.decode()) == paths[i]
        assert dev.capacity == CAP and dev.base_offset == i * CAP

    # ABI, alignment, target-full failure and connection reuse.
    desc = lib.alloc_on_device(CAP, UMM_TIER_SSD, len(paths) - 1)
    assert (desc.base_gpa >> 58) == owner
    assert desc.base_gpa & ((1 << 56) - 1) == (len(paths) - 1) * CAP
    for device, size, expected in [(len(paths) - 1, PAGE, -3), (len(paths), PAGE, -1),
                                   (0, (1 << 64) - 1, -1)]:
        from bmpclient.umm_client import ChunkDescriptor
        out = ChunkDescriptor()
        rc = lib._fn_alloc_on_device(size, UMM_TIER_SSD, device, ctypes.byref(out))
        assert rc == expected, (rc, expected)
    other = lib.alloc_tiered(PAGE, UMM_TIER_SSD)
    assert (other.base_gpa & ((1 << 56) - 1)) < CAP
    lib.free(other)
    lib.free(desc)

    # Reserve an initial page so extent bases are nonzero on every device.
    guards = [lib.alloc_on_device(PAGE, UMM_TIER_SSD, d) for d in range(len(paths))]
    vm = VirtualMedia(lib, unit_size=PAGE, capacity_per_device=64 * PAGE,
                      sp_bytes=4 * PAGE, sp_bytes_per_device={})
    store = SparseKVStore(vm, num_layers=2, max_tokens=64, max_topk=32)
    try:
        assert vm.device_count == len(paths)
        for layer in range(2):
            data = b''.join(bytes([1 + layer * 64 + t]) * PAGE for t in range(32))
            store.offload([KVBlockRef(layer, 0, 0, 32, memoryview(data))])
        store.flush()
        for layer in range(2):
            indices = [31, 0, 15, 8, 23, 2]
            outs = [memoryview(bytearray(PAGE)) for _ in indices]
            store.fetch(layer, indices, outs)
            for t, buf in zip(indices, outs):
                expected = bytes([1 + layer * 64 + t]) * PAGE
                assert bytes(buf) == expected
                device = vm.locate((layer, t))
                offset, _ = store.slot_table.locate(layer, t)
                # Independent disk-file check; extent starts after guard page.
                with paths[device].open("rb") as disk:
                    disk.seek(PAGE + offset)
                    assert disk.read(PAGE) == expected
        for layer in range(2):
            store.release(layer, list(range(32)))
    finally:
        store.close()
        for guard in guards:
            lib.free(guard)
    print(f"PASS: {len(paths)} devices, owner={owner}, allocation + SparseKVStore + disk placement")


def run(mode):
    with tempfile.TemporaryDirectory(prefix=f"umm_alloc_{mode}_") as directory:
        tmp = Path(directory)
        # 16 SSDs + CXL exercises a topology larger than one control frame.
        paths = [tmp / f"ssd{i}.raw" for i in range(16 if mode == "rpc" else 2)]
        lib = UMMLib()
        cfg = UMMConfig()
        cfg.memory_size = CAP
        cfg.my_node_id = 3
        cfg.ssd_owner_node = UMM_NODE_UNKNOWN
        if mode == "direct":
            assert lib.init(cfg) == 0
            try:
                for path in paths:
                    lib._lib.umm_register_storage_tier.argtypes = [ctypes.c_uint8, ctypes.c_char_p, ctypes.c_uint64]
                    assert lib._lib.umm_register_storage_tier(UMM_TIER_SSD, str(path).encode(), CAP) == 0
                check(lib, paths, 3)
            finally:
                lib.deinit()
        else:
            with servers(tmp, paths) as (meta_port, mem_port):
                cfg.meta_server_addr = f"127.0.0.1:{meta_port}".encode()
                cfg.mem_server_addr = f"127.0.0.1:{mem_port}".encode()
                if mode == "rpc":
                    cfg.num_ssd_devices = len(paths)
                    for i, path in enumerate(paths):
                        cfg.ssd_devices[i].path = str(path).encode()
                        cfg.ssd_devices[i].size = CAP
                else:
                    cfg.peer_nodes = f"{OWNER}:127.0.0.1:{mem_port}".encode()
                assert lib.init(cfg) == 0
                try:
                    check(lib, paths, OWNER)
                finally:
                    lib.deinit()


def check_legacy_rpc():
    """An old server's empty unknown-op response must not cause tier fallback."""
    wire = struct.Struct("<4sBBBBH6s")
    errors = []
    def recv_exact(conn, size):
        result = b""
        while len(result) < size:
            chunk = conn.recv(size - len(result))
            if not chunk:
                raise EOFError("short control frame")
            result += chunk
        return result

    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(5)
        def serve():
            try:
                with listener.accept()[0] as conn:
                    conn.settimeout(5)
                    for expected in (12, 5):
                        magic, ver, op, flags, reserved, length, token = wire.unpack(recv_exact(conn, 16))
                        body = recv_exact(conn, length)
                        assert magic == b"UMMR" and op == expected
                        if op == 12:
                            assert struct.unpack("<BIQI", body) == (2, 5, PAGE, 0)
                        response = b"" if op == 12 else struct.pack("<iQB", 0, 0, OWNER)
                        conn.sendall(wire.pack(magic, ver, op, 1, 0, len(response), token) + response)
            except BaseException as exc:
                errors.append(exc)
        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        native = UMMLib()._lib
        client = ctypes.create_string_buffer(512)
        native.mem_rpc_client_init.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        native.mem_rpc_client_deinit.argtypes = [ctypes.c_void_p]
        native.mem_rpc_alloc_on_device.argtypes = [ctypes.c_void_p, ctypes.c_uint8,
            ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint8)]
        native.mem_rpc_alloc_tiered2.argtypes = [ctypes.c_void_p, ctypes.c_uint8,
            ctypes.c_uint64, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64),
            ctypes.POINTER(ctypes.c_uint8)]
        assert native.mem_rpc_client_init(client, b"127.0.0.1", listener.getsockname()[1]) == 0
        try:
            off, owner = ctypes.c_uint64(), ctypes.c_uint8()
            assert native.mem_rpc_alloc_on_device(client, 2, 5, PAGE, 0,
                ctypes.byref(off), ctypes.byref(owner)) == -9
            assert native.mem_rpc_alloc_tiered2(client, 2, PAGE, 0,
                ctypes.byref(off), ctypes.byref(owner)) == 0
            assert owner.value == OWNER
        finally:
            native.mem_rpc_client_deinit(client)
            worker.join(timeout=6)
        assert not worker.is_alive() and not errors, errors
    print("PASS: legacy RPC reports unsupported; connection remains usable")


def check_bad_device_config():
    with tempfile.TemporaryDirectory(prefix="umm_bad_device_") as directory:
        tmp = Path(directory)
        (tmp / "missing").write_text("not a directory")
        config = tmp / "umms.yaml"
        config.write_text(f'listen_addr: "127.0.0.1"\nlisten_port: {free_port()}\n'
            f'memory_size: {CAP}\nssd_devices: "{tmp}/first.raw:{CAP},'
            f'{tmp}/missing/disk.raw:{CAP},{tmp}/last.raw:{CAP}"\n')
        result = subprocess.run([str(ROOT / "umm/bin/umms"), "-c", str(config)],
                                capture_output=True, timeout=8)
        assert result.returncode != 0
        assert b"failed to register SSD device[1]" in result.stderr
        assert not (tmp / "last.raw").exists()
    print("PASS: failed device registration aborts startup without shifting device indices")


if __name__ == "__main__":
    for mode in ("direct", "rpc", "remote"):
        print(f"Testing {mode}", flush=True)
        run(mode)
    check_legacy_rpc()
    check_bad_device_config()
