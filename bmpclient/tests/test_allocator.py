import os
import subprocess
import sys
import threading
import time
import unittest

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UMM_ROOT = os.path.join(PROJECT_ROOT, "UMM")
sys.path.insert(0, PROJECT_ROOT)

from bmpclient.allocator import FineGrainedAllocator, MemoryExhaustedError


META_PORT = 20001
MEM_PORT = 20002
META_ADDR = f"127.0.0.1:{META_PORT}"
MEM_ADDR = f"127.0.0.1:{MEM_PORT}"
SSD_DIR = "/tmp/umm_test_ssd"
SSD_MOCK_DEVICE = "/tmp/umm_mock_ssd.raw"


class TestAllocator(unittest.TestCase):
    _ummd_proc = None
    _umms_proc = None

    @classmethod
    def setUpClass(cls):
        os.makedirs(SSD_DIR, exist_ok=True)
        cls._devnull = open(os.devnull, "w")

        ummd_bin = os.path.join(UMM_ROOT, "bin", "umm-metadata-service")
        umms_bin = os.path.join(UMM_ROOT, "bin", "umm-memory-server")

        cls._ummd_proc = subprocess.Popen(
            [ummd_bin, "-p", str(META_PORT), "-b", "127.0.0.1"],
            stdout=cls._devnull, stderr=cls._devnull,
        )
        time.sleep(0.5)

        cls._umms_proc = subprocess.Popen(
            [umms_bin, "-p", str(MEM_PORT), "-b", "127.0.0.1",
             "-n", "0", "-s", str(128 * 1024 * 1024), "-d", SSD_DIR],
            stdout=cls._devnull, stderr=cls._devnull,
        )
        time.sleep(0.5)

        for _ in range(20):
            import socket
            try:
                with socket.create_connection(("127.0.0.1", META_PORT), timeout=0.5):
                    with socket.create_connection(("127.0.0.1", MEM_PORT), timeout=0.5):
                        break
            except Exception:
                time.sleep(0.2)
        else:
            cls.tearDownClass()
            raise RuntimeError("UMM services failed to start")

    @classmethod
    def tearDownClass(cls):
        if cls._umms_proc:
            cls._umms_proc.terminate()
            try:
                cls._umms_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                cls._umms_proc.kill()
                cls._umms_proc.wait()
        if cls._ummd_proc:
            cls._ummd_proc.terminate()
            try:
                cls._ummd_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                cls._ummd_proc.kill()
                cls._ummd_proc.wait()
        cls._devnull.close()

    def setUp(self):
        pass  # UMM chunk 状态由每个测试自行管理，通过 free/close 清理

    def test_alloc_and_free(self):
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        block = allocator.alloc(1024, "dram")
        self.assertIsInstance(block.chunk_id, int)
        self.assertGreater(block.chunk_id, 0)
        self.assertEqual(block.offset, 0)
        self.assertEqual(block.size, 1024)
        allocator.free(block)
        allocator.close()

    def test_alloc_multiple_in_same_chunk(self):
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        b1 = allocator.alloc(1024, "dram")
        b2 = allocator.alloc(1024, "dram")
        self.assertEqual(b1.chunk_id, b2.chunk_id)
        self.assertEqual(b1.offset, 0)
        self.assertEqual(b2.offset, 1024)
        allocator.free(b1)
        allocator.free(b2)
        allocator.close()

    def test_alloc_cross_chunks(self):
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        blocks = [allocator.alloc(1024 * 1024, "dram") for _ in range(3)]
        chunk_ids = {b.chunk_id for b in blocks}
        self.assertGreaterEqual(len(chunk_ids), 2)
        for b in blocks:
            allocator.free(b)
        allocator.close()

    def test_chunk_recycle(self):
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        block = allocator.alloc(1024 * 1024, "dram")
        chunk_id = block.chunk_id
        allocator.free(block)
        self.assertNotIn(chunk_id, allocator.chunks_by_type["dram"])
        allocator.close()

    def test_preallocate_chunks(self):
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        allocator.preallocate_chunks(2, 2 * 1024 * 1024, "dram")
        self.assertEqual(len(allocator.chunks_by_type["dram"]), 2)
        allocator.close()

    def test_dram_ssd_isolation(self):
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        dram_blocks = []
        for _ in range(200):
            try:
                dram_blocks.append(allocator.alloc(2 * 1024 * 1024, "dram"))
            except MemoryExhaustedError:
                break
        with self.assertRaises(MemoryExhaustedError):
            allocator.alloc(1024, "dram")
        # SSD 仍可分配
        block = allocator.alloc(1024, "ssd")
        self.assertIsInstance(block.chunk_id, int)
        self.assertGreater(block.chunk_id, 0)
        for b in dram_blocks:
            allocator.free(b)
        allocator.free(block)
        allocator.close()

    def test_memory_exhausted(self):
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        blocks = []
        for _ in range(200):
            try:
                blocks.append(allocator.alloc(2 * 1024 * 1024, "dram"))
            except MemoryExhaustedError:
                break
        with self.assertRaises(MemoryExhaustedError):
            allocator.alloc(1024, "dram")
        for b in blocks:
            allocator.free(b)
        allocator.close()

    def test_thread_safety(self):
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        errors = []
        lock = threading.Lock()

        def worker():
            try:
                for _ in range(10):
                    b = allocator.alloc(1024, "dram")
                    allocator.free(b)
            except Exception as e:
                with lock:
                    errors.append(e)

        threads = [threading.Thread(target=worker) for _ in range(5)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        self.assertEqual(len(errors), 0)
        allocator.close()

    # ------------------------------------------------------------------
    # 新增：基于 Block 的 read / write 接口测试
    # ------------------------------------------------------------------

    def test_block_read_write(self):
        """基本读写：先 write 再 read，数据应一致。"""
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        block = allocator.alloc(4096, "dram")
        data = b"hello umm read/write"
        allocator.write(block, 0, data)
        result = allocator.read(block, 0, len(data))
        self.assertEqual(result, data)
        allocator.free(block)
        allocator.close()

    def test_block_read_write_with_offset(self):
        """带偏移的读写：在 Block 中间位置写入后读取。"""
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        block = allocator.alloc(4096, "dram")
        prefix = b"A" * 1024
        payload = b"offset payload"
        suffix = b"B" * 1024
        allocator.write(block, 0, prefix)
        allocator.write(block, 1024, payload)
        allocator.write(block, 1024 + len(payload), suffix)

        self.assertEqual(allocator.read(block, 0, len(prefix)), prefix)
        self.assertEqual(allocator.read(block, 1024, len(payload)), payload)
        self.assertEqual(allocator.read(block, 1024 + len(payload), len(suffix)), suffix)
        allocator.free(block)
        allocator.close()

    def test_block_read_write_ssd(self):
        """SSD 介质上的读写。"""
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        block = allocator.alloc(4096, "ssd")
        data = b"ssd block data"
        allocator.write(block, 0, data)
        result = allocator.read(block, 0, len(data))
        self.assertEqual(result, data)
        allocator.free(block)
        allocator.close()

    def test_block_read_out_of_range(self):
        """读取范围超出 Block 大小应抛 ValueError。"""
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        block = allocator.alloc(1024, "dram")
        allocator.write(block, 0, b"x" * 1024)
        with self.assertRaises(ValueError):
            allocator.read(block, 512, 1024)  # 512 + 1024 > 1024
        with self.assertRaises(ValueError):
            allocator.read(block, 1024, 1)    # offset == block.size
        allocator.free(block)
        allocator.close()

    def test_block_write_out_of_range(self):
        """写入范围超出 Block 大小应抛 ValueError。"""
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        block = allocator.alloc(1024, "dram")
        with self.assertRaises(ValueError):
            allocator.write(block, 512, b"y" * 1024)  # 512 + 1024 > 1024
        allocator.free(block)
        allocator.close()

    def test_block_read_write_entire_block(self):
        """读写整个 Block（默认 size=None 应读到 Block 末尾）。"""
        allocator = FineGrainedAllocator(META_ADDR, MEM_ADDR, ssd_device=SSD_MOCK_DEVICE, default_chunk_size=2 * 1024 * 1024)
        block = allocator.alloc(2048, "dram")
        data = bytes(range(256)) * 8  # 2048 bytes
        allocator.write(block, 0, data)
        result = allocator.read(block)  # offset=0, size=None
        self.assertEqual(result, data)
        allocator.free(block)
        allocator.close()


if __name__ == "__main__":
    unittest.main()
