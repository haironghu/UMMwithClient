"""Device CLI checks using ordinary temporary files and mocked native opens only."""
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from bmpclient.bench.layout_replay import parser, run


class TestLayoutDevice(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='umm-device-cli-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.trace = self.root / 'trace.jsonl'
        self.trace.write_text(json.dumps(dict(request_id='r', step_id=0, layer_id=0,
                                            context_length=4, topk_token_indices=[3, 0])))
        self.target = self.root / 'ordinary-file'
        self.target.write_bytes(b'never overwrite')
        self.output = self.root / 'result.json'

    def args(self, *extra):
        return parser().parse_args(['--trace', str(self.trace), '--device', str(self.target),
            '--output', str(self.output), '--segments', '4K', '--repeats', '1', *extra])

    def test_dry_run_never_opens_target_or_creates_results(self):
        with patch('bmpclient.bench.layout_replay.DirectPool') as pool, contextlib.redirect_stdout(io.StringIO()):
            result = run(self.args('--window-offset', '64K', '--window-bytes', '1M', '--dry-run'))
        pool.assert_not_called()
        self.assertEqual(result['status'], 'planned')
        self.assertEqual(result['possible_write_range'], [65536, 65536 + 16384])
        self.assertFalse(self.output.exists())
        self.assertEqual(self.target.read_bytes(), b'never overwrite')

    def test_missing_authorization_invalid_windows_and_regular_device_rejected(self):
        cases = [[], ['--window-offset', '0', '--window-bytes', '1M'],
                 ['--allow-device-write', '--window-offset', '1', '--window-bytes', '1M'],
                 ['--allow-device-write', '--window-offset', '0', '--window-bytes', '4K'],
                 ['--allow-device-write', '--window-offset', str(1 << 63), '--window-bytes', '1M'],
                 ['--allow-device-write', '--window-offset', '0', '--window-bytes', '1M']]
        with patch('bmpclient.bench.layout_replay.DirectPool') as pool:
            for extra in cases:
                with self.subTest(extra=extra), self.assertRaises(ValueError):
                    run(self.args(*extra))
                self.assertFalse(self.output.exists())
        pool.assert_not_called()
        self.assertEqual(self.target.read_bytes(), b'never overwrite')

    def test_window_forwarding_no_preallocation_env_restored_and_failure_recorded(self):
        def fail_open(target, base, capacity):
            self.assertEqual((target, base, capacity), (self.target.resolve(), 65536, 1048576))
            self.assertEqual(os.environ['UMM_ALLOW_BLOCK_DEVICE'], '1')
            raise RuntimeError('simulated exclusive open rejection')
        with patch.dict(os.environ, {'UMM_ALLOW_BLOCK_DEVICE': 'previous'}), \
             patch('bmpclient.bench.layout_replay.stat.S_ISBLK', return_value=True), \
             patch('bmpclient.bench.layout_replay.DirectPool', side_effect=fail_open), \
             patch('bmpclient.bench.layout_replay.os.posix_fallocate') as allocate:
            with self.assertRaisesRegex(RuntimeError, 'simulated'):
                run(self.args('--allow-device-write', '--window-offset', '64K', '--window-bytes', '1M'))
            self.assertEqual(os.environ['UMM_ALLOW_BLOCK_DEVICE'], 'previous')
            allocate.assert_not_called()
        result = json.loads(self.output.read_text())
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['target_kind'], 'block_device')
        self.assertEqual(self.target.read_bytes(), b'never overwrite')


if __name__ == '__main__':
    unittest.main()
