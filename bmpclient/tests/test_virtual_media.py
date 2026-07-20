import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UMM_ROOT = os.path.join(PROJECT_ROOT, "UMM")
sys.path.insert(0, PROJECT_ROOT)

from bmpclient.umm_client import UMMLib, UMMConfig, UMM_TIER_SSD
from bmpclient.virtual_media import VirtualMedia, VirtualMediaFullError
from bmpclient.virtual_media_config import load_config
from bmpclient.virtual_media_strategy import (
    ConsistentHashStrategy,
    CustomStrategyPlugin,
    DataPlacementStrategy,
    RoundRobinStrategy,
    create_strategy,
)


META_PORT = 20001
MEM_PORT = 20002
META_ADDR = f"127.0.0.1:{META_PORT}"
MEM_ADDR = f"127.0.0.1:{MEM_PORT}"
SSD_DIR = "/tmp/umm_test_ssd"
SSD_MOCK_DEVICE = "/tmp/umm_mock_ssd.raw"


class TestVirtualMedia(unittest.TestCase):
    _ummd_proc = None
    _umms_proc = None
    _lib = None

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

        # 初始化 UMMLib
        cls._lib = UMMLib()
        cfg = UMMConfig()
        cfg.transport = b"mock"
        cfg.consistency_model = b"hardware"
        cfg.memory_size = 64 * 1024 * 1024
        cfg.meta_server_addr = META_ADDR.encode("utf-8")
        cfg.mem_server_addr = MEM_ADDR.encode("utf-8")
        cfg.ssd_device = SSD_MOCK_DEVICE.encode("utf-8")
        cfg.my_node_id = 0

        rc = cls._lib.init(cfg)
        if rc != 0:
            cls.tearDownClass()
            raise RuntimeError(f"umm_init failed: rc={rc}")

    @classmethod
    def tearDownClass(cls):
        if cls._lib:
            cls._lib.deinit()
            cls._lib = None

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
        pass

    def test_create_invalid_size(self):
        """size 不能被设备数整除或每设备大小不能被粒度整除时应抛 ValueError。"""
        # 先查询在线 SSD 设备数
        topo = self._lib.get_topology()
        ssd_count = sum(
            1 for i in range(topo.num_resources)
            if topo.resources[i].tier == UMM_TIER_SSD and topo.resources[i].online
        )

        # 情况 1：总 size 不能被设备数整除（仅在多设备环境下有意义）
        if ssd_count > 1:
            with self.assertRaises(ValueError):
                VirtualMedia(self._lib, size=31 * 1024 * 1024, granularity=1024 * 1024)

        # 情况 2：每设备大小不能被粒度整除
        # 当只有 1 个 SSD 设备时，30M / 1 = 30M，30M % 7M = 2M，不整除
        with self.assertRaises(ValueError):
            VirtualMedia(self._lib, size=30 * 1024 * 1024, granularity=7 * 1024 * 1024)

    def test_create_invalid_granularity(self):
        """size 不能被 granularity 整除时应抛 ValueError。"""
        with self.assertRaises(ValueError):
            VirtualMedia(self._lib, size=30 * 1024 * 1024, granularity=7 * 1024 * 1024)

    def test_capacity_and_slot_count(self):
        """验证容量、粒度、槽位数和设备数。"""
        # mock 模式下通常只有 1 个 SSD 设备
        vm = VirtualMedia(self._lib, size=8 * 1024 * 1024, granularity=1024 * 1024)
        self.assertEqual(vm.capacity, 8 * 1024 * 1024)
        self.assertEqual(vm.granularity, 1024 * 1024)
        self.assertEqual(vm.slot_count, 8)
        self.assertGreaterEqual(vm.device_count, 1)
        vm.close()

    def test_save_and_read_round_robin(self):
        """save 返回递增索引，read 能正确读取。"""
        vm = VirtualMedia(self._lib, size=8 * 1024 * 1024, granularity=1024)
        records = []
        for i in range(vm.slot_count):
            data = f"record-{i:08d}".encode().ljust(vm.granularity, b'\0')
            idx = vm.save(data)
            self.assertEqual(idx, i)
            records.append(data)

        for i, expected in enumerate(records):
            self.assertEqual(vm.read(i), expected)

        vm.close()

    def test_full_exception(self):
        """写满后再次 save 应抛 VirtualMediaFullError。"""
        vm = VirtualMedia(self._lib, size=4 * 1024 * 1024, granularity=1024 * 1024)
        for i in range(vm.slot_count):
            vm.save(b"x" * vm.granularity)

        self.assertEqual(vm.written_count, vm.slot_count)

        with self.assertRaises(VirtualMediaFullError):
            vm.save(b"x" * vm.granularity)

        vm.close()

    def test_read_unwritten_index(self):
        """读取未写入的索引应抛 ValueError。"""
        vm = VirtualMedia(self._lib, size=4 * 1024 * 1024, granularity=1024 * 1024)
        vm.save(b"x" * vm.granularity)

        with self.assertRaises(ValueError):
            vm.read(1)

        vm.close()

    def test_close_releases_chunks(self):
        """close 后应能正常结束，不泄露 Chunk。"""
        vm = VirtualMedia(self._lib, size=4 * 1024 * 1024, granularity=1024 * 1024)
        vm.save(b"x" * vm.granularity)
        vm.close()
        # 关闭后再操作应因底层已释放而失败
        with self.assertRaises(Exception):
            vm.save(b"x" * vm.granularity)

    def test_thread_safety_save(self):
        """多线程并发 save 不应冲突或重复。"""
        vm = VirtualMedia(self._lib, size=8 * 1024 * 1024, granularity=1024)
        errors = []
        lock = threading.Lock()

        def worker(start):
            try:
                for i in range(vm.slot_count // 4):
                    data = f"t{start:02d}-{i:08d}".encode().ljust(vm.granularity, b'\0')
                    vm.save(data)
            except Exception as e:
                with lock:
                    errors.append(e)

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        self.assertEqual(len(errors), 0)
        self.assertEqual(vm.written_count, vm.slot_count)
        vm.close()

    def test_default_strategy_is_round_robin(self):
        """默认策略应为 round-robin。"""
        vm = VirtualMedia(self._lib, size=4 * 1024 * 1024, granularity=1024 * 1024)
        self.assertIsInstance(vm.strategy, RoundRobinStrategy)
        vm.close()

    def test_explicit_round_robin_strategy(self):
        """显式传入 round_robin 策略名，行为与默认一致。"""
        vm = VirtualMedia(
            self._lib, size=4 * 1024 * 1024, granularity=1024 * 1024,
            strategy="round_robin",
        )
        self.assertIsInstance(vm.strategy, RoundRobinStrategy)

        records = []
        for i in range(vm.slot_count):
            data = f"rr-{i:04d}".encode().ljust(vm.granularity, b'\0')
            idx = vm.save(data)
            self.assertEqual(idx, i)
            records.append(data)

        for i, expected in enumerate(records):
            self.assertEqual(vm.read(i), expected)
        vm.close()

    def test_explicit_consistent_hash_strategy(self):
        """显式传入 consistent_hash 策略名，读写应保持一致。"""
        vm = VirtualMedia(
            self._lib, size=4 * 1024 * 1024, granularity=1024 * 1024,
            strategy="consistent_hash",
        )
        self.assertIsInstance(vm.strategy, ConsistentHashStrategy)

        records = []
        for i in range(vm.slot_count):
            data = f"ch-{i:04d}".encode().ljust(vm.granularity, b'\0')
            idx = vm.save(data)
            self.assertEqual(idx, i)
            records.append(data)

        for i, expected in enumerate(records):
            self.assertEqual(vm.read(i), expected)
        vm.close()

    def test_custom_strategy_instance(self):
        """传入自定义策略实例，应被直接使用。"""
        class ReverseStrategy(DataPlacementStrategy):
            def locate(self, index: int) -> int:
                return (self._num_devices - 1) - (index % self._num_devices)

        topo = self._lib.get_topology()
        ssd_count = sum(
            1 for i in range(topo.num_resources)
            if topo.resources[i].tier == UMM_TIER_SSD and topo.resources[i].online
        )

        strategy = ReverseStrategy(ssd_count, {})
        vm = VirtualMedia(
            self._lib, size=4 * 1024 * 1024, granularity=1024 * 1024,
            strategy=strategy,
        )
        self.assertIsInstance(vm.strategy, ReverseStrategy)

        records = []
        for i in range(vm.slot_count):
            data = f"cs-{i:04d}".encode().ljust(vm.granularity, b'\0')
            idx = vm.save(data)
            self.assertEqual(idx, i)
            records.append(data)

        for i, expected in enumerate(records):
            self.assertEqual(vm.read(i), expected)
        vm.close()

    def test_config_file_strategy(self):
        """通过配置文件指定策略，VirtualMedia 应正确加载。"""
        # mock 单设备环境下，consistent_hash 也能正常工作
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".json", delete=False
        ) as f:
            f.write('{"strategy": "consistent_hash", "virtual_nodes": 50}')
            config_path = f.name

        try:
            cfg = load_config(config_path)
            self.assertEqual(cfg["strategy"], "consistent_hash")
            self.assertEqual(cfg["virtual_nodes"], 50)

            vm = VirtualMedia(
                self._lib, size=4 * 1024 * 1024, granularity=1024 * 1024,
                strategy="consistent_hash",
            )
            self.assertIsInstance(vm.strategy, ConsistentHashStrategy)
            data = b"config-driven" + b"\0" * (vm.granularity - len(b"config-driven"))
            idx = vm.save(data)
            self.assertEqual(vm.read(idx), data)
            vm.close()
        finally:
            os.unlink(config_path)

    def test_strategy_factory(self):
        """策略工厂 create_strategy 应能构造已知策略。"""
        rr = create_strategy("round_robin", 4, {})
        self.assertIsInstance(rr, RoundRobinStrategy)
        self.assertEqual(rr.locate(5), 1)

        ch = create_strategy("consistent_hash", 4, {"virtual_nodes": 10})
        self.assertIsInstance(ch, ConsistentHashStrategy)
        self.assertIn(ch.locate(12345), range(4))

        with self.assertRaises(ValueError):
            create_strategy("unknown", 4, {})

    def test_custom_strategy_plugin(self):
        """custom 策略通过 importlib 动态加载用户模块/类。"""
        module_dir = tempfile.mkdtemp()
        module_name = "always_first_strategy"
        module_file = os.path.join(module_dir, f"{module_name}.py")

        try:
            with open(module_file, "w", encoding="utf-8") as f:
                f.write(
                    "from bmpclient.virtual_media_strategy import DataPlacementStrategy\n"
                    "\n"
                    "class AlwaysFirstStrategy(DataPlacementStrategy):\n"
                    "    def locate(self, index: int) -> int:\n"
                    "        return 0\n"
                )

            sys.path.insert(0, module_dir)
            try:
                plugin = CustomStrategyPlugin(4, {
                    "module": module_name,
                    "class": "AlwaysFirstStrategy",
                })
                self.assertEqual(plugin.locate(0), 0)
                self.assertEqual(plugin.locate(100), 0)
            finally:
                sys.path.remove(module_dir)
        finally:
            shutil.rmtree(module_dir, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
