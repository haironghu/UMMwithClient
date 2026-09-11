import csv
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from bmpclient.bench.convert_trace import convert
from bmpclient.bench.single_ssd import load_trace


class TestCSVTrace(unittest.TestCase):
    def csv(self, header, rows):
        stream = io.StringIO(newline='')
        writer = csv.writer(stream)
        writer.writerow(header)
        writer.writerows(rows)
        return stream.getvalue()

    def test_full_2048_columns_cli_preserves_order_and_duplicates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / 'trace.csv', root / 'trace.jsonl'
            header = ['step_idx', 'layer', 'step_seqlen'] + [f'idx_{i}' for i in range(2048)]
            tokens = [2047 - i for i in range(2048)]
            tokens[-1] = tokens[0]
            # Deliberately reorder CSV columns: order of idx_N, not position,
            # defines the output. Also exercise Excel UTF-8 BOM and CRLF.
            row = [7, 3, 4096] + tokens
            permutation = sorted(range(len(header)), key=lambda i: header[i])
            source.write_bytes(('\ufeff' + self.csv([header[i] for i in permutation],
                [[row[i] for i in permutation], [row[i] for i in permutation]])).encode('utf-8'))
            script = Path(__file__).resolve().parents[1] / 'scripts/convert_kv_trace.py'
            subprocess.run([sys.executable, str(script), '--input', str(source), '--output', str(output),
                            '--format', 'csv', '--request-id', 'r0'], check=True, capture_output=True, text=True)
            trace = load_trace(output, 2048)
            self.assertEqual(len(trace.records), 2)
            self.assertEqual(trace.records[0], dict(request_id='r0', step_id=7, layer_id=3,
                context_length=4096, topk_token_indices=tokens))

    def test_invalid_csv_does_not_create_output(self):
        good_header = ['step_idx', 'layer', 'step_seqlen', 'idx_0', 'idx_1']
        good_row = [0, 0, 8, 1, 7]
        cases = [(good_header[:-1], [good_row[:-1]]),
                 (good_header + ['idx_1'], [good_row + [2]]),
                 (good_header + ['idx_2'], [good_row + [2]]),
                 (good_header, [good_row[:-1]]),
                 (good_header, [])]
        for bad in ['', 'NaN', '2.0', 'true', '-1', '8']:
            cases.append((good_header, [[0, 0, 8, 1, bad]]))
        cases.extend([(good_header, [[-1, 0, 8, 1, 2]]), (good_header, [[0, 0, 0, 1, 2]])])
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / 'in.csv', Path(directory) / 'out.jsonl'
            for header, rows in cases:
                with self.subTest(header=header, rows=rows):
                    source.write_text(self.csv(header, rows))
                    with self.assertRaises(ValueError):
                        convert(source, output, defaults={'request_id': 'r'}, max_topk=2, input_format='csv')
                    self.assertFalse(output.exists())
            source.write_text(self.csv(good_header, [good_row]))
            with self.assertRaisesRegex(ValueError, 'request-id'):
                convert(source, output, max_topk=2, input_format='csv')
            with self.assertRaisesRegex(ValueError, 'fixed columns'):
                convert(source, output, {'step_id': 'x'}, {'request_id': 'r'}, max_topk=2, input_format='csv')
            convert(source, output, defaults={'request_id': 'r'}, max_topk=2, input_format='csv')
            saved = output.read_bytes()
            with self.assertRaises(FileExistsError):
                convert(source, output, defaults={'request_id': 'r'}, max_topk=2, input_format='csv')
            self.assertEqual(output.read_bytes(), saved)


if __name__ == '__main__':
    unittest.main()
