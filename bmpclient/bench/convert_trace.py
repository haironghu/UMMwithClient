"""Convert CSV, JSON/JSONL records or token lists into validated trace JSONL."""
import argparse
import csv
import hashlib
import io
import json
import re
from pathlib import Path
import tempfile

from bmpclient.bench.single_ssd import load_trace

FIELDS = ('request_id', 'step_id', 'layer_id', 'context_length', 'topk_token_indices')


def csv_records(raw, defaults, max_topk):
    """One CSV is one request; idx columns are ordered numerically, not lexically."""
    request = defaults.get('request_id')
    if not isinstance(request, str) or not request.strip():
        raise ValueError('CSV requires --request-id: one CSV represents one request')
    reader = csv.reader(io.StringIO(raw.decode('utf-8-sig'), newline=''), strict=True)
    try:
        header = [name.strip() for name in next(reader)]
    except StopIteration:
        raise ValueError('CSV is empty') from None
    if len(set(header)) != len(header):
        raise ValueError('CSV has duplicate column names')
    required = ['step_idx', 'layer', 'step_seqlen'] + [f'idx_{i}' for i in range(max_topk)]
    missing = set(required) - set(header)
    extra = set(header) - set(required)
    if missing or extra:
        raise ValueError(f'CSV header must contain step_idx, layer, step_seqlen, idx_0..idx_{max_topk - 1}; '
                         f'missing={sorted(missing)[:6]}, unexpected={sorted(extra)[:6]}')
    positions = {name: i for i, name in enumerate(header)}
    for cells in reader:
        if not cells:
            continue
        line = reader.line_num
        if len(cells) != len(header):
            raise ValueError(f'CSV line {line}: expected {len(header)} cells, got {len(cells)}')
        def number(name):
            value = cells[positions[name]].strip()
            if not re.fullmatch(r'[0-9]+', value):
                raise ValueError(f'CSV line {line}, {name}: expected a nonnegative integer, got {value!r}')
            return int(value)
        context = number('step_seqlen')
        if context < 1:
            raise ValueError(f'CSV line {line}: step_seqlen must be positive')
        tokens = [number(f'idx_{i}') for i in range(max_topk)]
        for i, token in enumerate(tokens):
            if token >= context:
                raise ValueError(f'CSV line {line}, idx_{i}: token must be < step_seqlen ({context})')
        yield dict(request_id=request, step_id=number('step_idx'), layer_id=number('layer'),
                   context_length=context, topk_token_indices=tokens)


def convert(source, output, field_map=None, defaults=None, step_start=0, max_topk=2048, input_format='jsonl'):
    if output.exists() or source.resolve() == output.resolve():
        raise FileExistsError('output must be new and different from input')
    if step_start < 0 or max_topk < 1:
        raise ValueError('step-start must be nonnegative; max-topk must be positive')
    field_map, defaults = field_map or {}, defaults or {}
    if set(field_map) - set(FIELDS):
        raise ValueError('unknown canonical field in --field')
    raw = source.read_bytes()
    if input_format == 'csv':
        if field_map or set(defaults) - {'request_id'} or step_start:
            raise ValueError('CSV has fixed columns; only --request-id and --max-topk metadata options apply')
        records = csv_records(raw, defaults, max_topk)
    elif input_format == 'json':
        records = json.loads(raw)
        if not isinstance(records, list):
            raise ValueError('JSON input must be an array of records')
    elif input_format == 'jsonl':
        records = [json.loads(line) for line in raw.splitlines() if line.strip()]
    else:
        raise ValueError(f'unsupported input format: {input_format}')
    normalized = []
    for i, item in enumerate(records):
        if isinstance(item, list):
            item = {'topk_token_indices': item}
        if not isinstance(item, dict):
            raise ValueError(f'record {i}: expected object or token list')
        row = {}
        for canonical in FIELDS:
            source_field = field_map.get(canonical, canonical)
            if source_field in item:
                row[canonical] = item[source_field]
            elif canonical in defaults:
                row[canonical] = defaults[canonical]
            elif canonical == 'step_id':
                row[canonical] = step_start + i
            else:
                raise ValueError(f'record {i}: missing {canonical}; provide a field mapping or explicit metadata')
        normalized.append(row)
    # Use exactly the replay validator before creating the final output.
    text = ''.join(json.dumps(row, ensure_ascii=False) + '\n' for row in normalized)
    with tempfile.TemporaryDirectory(prefix='umm-convert-') as directory:
        temporary = Path(directory) / 'trace.jsonl'
        temporary.write_text(text, encoding='utf-8')
        trace = load_trace(temporary, max_topk)
    with output.open('x', encoding='utf-8') as stream:
        stream.write(text)
    return dict(records=len(normalized), kv_rows=trace.rows, trace_sha256=trace.digest,
                source_sha256=hashlib.sha256(raw).hexdigest())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--format', choices=['jsonl', 'json', 'csv'], default='jsonl')
    p.add_argument('--field', action='append', default=[], metavar='CANONICAL=SOURCE')
    p.add_argument('--request-id')
    p.add_argument('--layer-id', type=int)
    p.add_argument('--context-length', type=int)
    p.add_argument('--step-start', type=int, default=0)
    p.add_argument('--max-topk', type=int, default=2048,
                   help='maximum entries per JSON record; exact idx column count for CSV')
    args = p.parse_args()
    mapping = {}
    for field in args.field:
        if '=' not in field:
            p.error('--field requires CANONICAL=SOURCE')
        key, value = field.split('=', 1)
        if key in mapping or not value:
            p.error('duplicate canonical field or empty source field')
        mapping[key] = value
    defaults = {key: getattr(args, key) for key in ('request_id', 'layer_id', 'context_length')
                if getattr(args, key) is not None}
    print(json.dumps(convert(args.input, args.output, mapping, defaults, args.step_start,
                             args.max_topk, args.format), indent=2))


if __name__ == '__main__':
    main()
