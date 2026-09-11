"""Eight-device manifest and deterministic hash geometry for host replay."""
import json
from pathlib import Path
from bmpclient.bench.single_ssd import PAGE, size_arg
from bmpclient.virtual_media_strategy import PlacementStrategy, PositionHashStrategy


class OriginalLayerHash(PlacementStrategy):
    """Keep original model layer IDs in the hash despite dense slot-table IDs."""
    def __init__(self, count, original_layers):
        super().__init__(count, {})
        self.layers = original_layers
        self.hash = PositionHashStrategy(count, {})

    def locate(self, key):
        layer, token = key
        return self.hash.locate((self.layers[layer], token))


def hash_capacities(trace, segment, mode, count, lengths):
    strategy = PositionHashStrategy(count, {})
    requests = sorted({req for req, _ in lengths})
    sizes = {req: [0] * count for req in requests}
    rows = {req: [0] * count for req in requests}
    loaded = {}
    if mode == 'snapshot':
        events = [(req, layer, end) for (req, layer), end in sorted(lengths.items())]
    else:
        events = [(r['request_id'], r['layer_id'], r['context_length']) for r in trace.records]
    for req, layer, end in events:
        start = loaded.get((req, layer), 0)
        counts = [0] * count
        for token in range(start, end):
            counts[strategy.locate((layer, token))] += 1
        for d, n in enumerate(counts):
            rows[req][d] += n
            sizes[req][d] += ((n * PAGE + segment - 1) // segment) * segment if mode == 'online' else n * PAGE
        loaded[(req, layer)] = end
    for req in requests:
        # VM owns an extent on every device, even when that request has no rows
        # on one of them. Reserve at least one segment, but never fake a write.
        sizes[req] = [max(segment, ((n + segment - 1) // segment) * segment) for n in sizes[req]]
    return sizes, rows


def load_devices(path):
    entries = json.loads(path.read_text())
    if not isinstance(entries, list) or len(entries) != 8:
        raise ValueError('devices-config must be a JSON array of exactly eight windows')
    windows = []
    for d, entry in enumerate(entries):
        if not isinstance(entry, dict) or (('device' in entry) == ('file' in entry)):
            raise ValueError(f'device {d}: specify exactly one of device/file')
        kind = 'device' if 'device' in entry else 'file'
        if set(entry) != {kind, 'window_offset', 'window_bytes'}:
            raise ValueError(f'device {d}: expected {kind}, window_offset, window_bytes')
        if not isinstance(entry[kind], str) or not Path(entry[kind]).is_absolute():
            raise ValueError(f'device {d}: target must be an absolute path')
        values = []
        for key in ('window_offset', 'window_bytes'):
            value = entry[key]
            if type(value) not in (int, str):
                raise ValueError(f'device {d}: invalid {key}')
            values.append(size_arg(value))
        windows.append(dict(device_idx=d, kind=kind, target=Path(entry[kind]).resolve(),
                            window_offset=values[0], window_bytes=values[1]))
    if len({w['target'] for w in windows}) != 8:
        raise ValueError('duplicate device paths or symlink aliases')
    if len({w['kind'] for w in windows}) != 1:
        raise ValueError('do not mix block devices and test files')
    return windows
