"""Unit tests for the shared selection module and the second-generation flow.

Covers: shared ppa_selection import path used by the online hook, the
declared-stride layout contract, second-generation export (applied fields,
full declared-region snapshot), applied replay reproducing offline selections,
online-LRU digest cross-checks, and rejection of diverged/fallback traces.
MQSim itself is not needed.
"""
import json
from pathlib import Path
import tempfile
import unittest

from export_ppa_candidates import export, parser as export_parser
from make_ppa_oracle_fixture import make_fixture
import ppa_selection
from mqsim_ppa_oracle import baseline_states, selections_with_baseline, select, validate_export

STRIDE = 12000


class SharedModuleTests(unittest.TestCase):
    def test_select_importable_from_shared_module(self):
        mapping = {i: dict(lpa=i, ppa=i, channel=int(i >= 4), chip=0, die=0, plane=0, block=0, page=i)
                   for i in range(8)}
        rows = [dict(token=i, score=None if i == 0 else 20 - i, offset_bytes=i * 16384) for i in range(8)]
        chosen, swaps, before, after = ppa_selection.select({0, 1, 2, 3}, rows, mapping, 3, 1)
        self.assertEqual(len(chosen), 4)
        self.assertEqual(len(swaps), 1)
        self.assertLess(after, before)

    def test_declared_stride_offsets_and_lru_digest(self):
        # Rank is the position in ascending numeric-id order (prepare-time
        # contract); second-generation records carry the hook's layer_rank.
        offsets = ppa_selection.declared_stride_offsets([10, 2], STRIDE)
        self.assertEqual(offsets, {2: 0, 10: STRIDE * 4096})
        with self.assertRaises(ValueError):
            ppa_selection.declared_stride_offsets([2], 0)
        first = ppa_selection.BatchLRU(4); first.commit([3, 1])
        second = ppa_selection.BatchLRU(4); second.commit([1, 3])
        self.assertEqual(ppa_selection.lru_digest(first), ppa_selection.lru_digest(second))
        second.commit([2])
        self.assertNotEqual(ppa_selection.lru_digest(first), ppa_selection.lru_digest(second))


class SecondGenerationTests(unittest.TestCase):
    def mapping(self, pages):
        # 8 alternating channels so the greedy swap has something to spread over.
        return {lpa: dict(lpa=lpa, ppa=lpa, channel=lpa % 8, chip=0, die=0, plane=0, block=0, page=lpa)
                for lpa in range(pages)}

    def prepared(self, root, steps=2, layers=(2, 10), applied=True):
        """First-gen export with declared stride; optionally inject applied fields
        recomputed offline (the same role the online hook plays) and re-export."""
        make_fixture(root / 'sample', layers=layers, steps=steps)
        first = export(export_parser().parse_args(
            [str(root / 'sample'), '--out-dir', str(root / 'e1'),
             '--segment-bytes', '262144', '--declared-stride-tokens', str(STRIDE)]))
        # The fake mapping must cover both layouts: prepare-time position ranks
        # and the hook's numeric-id ranks injected below (full declared region).
        max_rank = max(max(layers), len(layers))
        mapping = self.mapping((max_rank + 1) * STRIDE * 4096 // 16384 + 1)
        planned = {}
        for row, cached, digest in baseline_states(root / 'e1', 4096):
            selected, swaps, _, _ = select(set(row['native']), row['candidates'], mapping,
                                           row['current_token'], 8, cached=cached)
            planned[(row['step_id'], row['layer_id'])] = (sorted(selected),
                                                          [[o, n] for o, n in swaps], digest)
        if not applied:
            return first, mapping, planned
        for path in (root / 'sample').glob('*.json'):
            raw = json.loads(path.read_text())
            step = raw['seq_lens_key'][0] - 10000
            layer = int(raw['layer'].split('layers.')[1].split('.')[0])
            selected, swaps, digest = planned[(step, layer)]
            raw.update(selected=selected, swaps=swaps, overridden=True, cache_mode='shared',
                       cache_capacity=4096, cache_state_sha256=digest,
                       layer_rank=layer, stride_tokens=STRIDE)
            path.write_text(json.dumps(raw))
        second = export(export_parser().parse_args(
            [str(root / 'sample'), '--out-dir', str(root / 'e2'), '--segment-bytes', '262144']))
        return second, mapping, planned

    def test_stride_export_covers_full_declared_region(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest, _, _ = self.prepared(root, applied=False)
            self.assertEqual(manifest['declared_stride_tokens'], STRIDE)
            self.assertFalse(manifest['has_applied_selections'])
            # Prepare-time ranks are positions (0 and 1); the snapshot covers
            # the whole declared region regardless of where contexts stop.
            self.assertEqual(manifest['snapshot_bytes'], 2 * STRIDE * 4096)
            geometry = {r['layer_id']: r['base_bytes'] for r in manifest['layers']}
            self.assertEqual(geometry[2], 0)
            self.assertEqual(geometry[10], STRIDE * 4096)
            rows = [json.loads(line) for line in (root / 'e1/candidates.jsonl').read_text().splitlines()]
            spot = next(r for r in rows if r['layer_id'] == 10)
            self.assertEqual(spot['candidates'][0]['offset_bytes'], STRIDE * 4096)

    def test_second_generation_export_carries_applied_selections(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest, _, _ = self.prepared(root)
            self.assertTrue(manifest['has_applied_selections'])
            self.assertEqual(manifest['declared_stride_tokens'], STRIDE)
            rows = [json.loads(line) for line in (root / 'e2/candidates.jsonl').read_text().splitlines()]
            self.assertTrue(all('applied_selected' in r for r in rows))
            self.assertTrue(all(len(r['applied_selected']) == len(r['native']) for r in rows))
            self.assertEqual(validate_export(root / 'e2')['records'], len(rows))

    def test_applied_replay_reproduces_online_selections(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _, mapping, planned = self.prepared(root)
            replayed = list(selections_with_baseline(root / 'e2', mapping, 4096,
                                                     'applied', 0, 0, 2026, 0))
            self.assertEqual(len(replayed), len(planned))
            for (mrow, audit, offsets), ((step, layer), (selected, swaps, digest)) in zip(
                    replayed, sorted(planned.items())):
                self.assertEqual((mrow['step'], mrow['layer']), (step, layer))
                self.assertEqual(audit['selected_indices'], selected)
                self.assertEqual([list(p) for p in audit['swaps']], swaps)
                self.assertEqual(mrow['replacements'], len(swaps))

    def test_applied_replay_rejects_diverged_online_lru(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest, mapping, _ = self.prepared(root)
            del manifest
            path = next((root / 'sample').glob('model_layers_2_s10001.json'))
            raw = json.loads(path.read_text())
            raw['cache_state_sha256'] = '0' * 64
            path.write_text(json.dumps(raw))
            export(export_parser().parse_args(
                [str(root / 'sample'), '--out-dir', str(root / 'e3'), '--segment-bytes', '262144']))
            with self.assertRaisesRegex(ValueError, 'Online LRU state differs'):
                list(selections_with_baseline(root / 'e3', mapping, 4096, 'applied', 0, 0, 2026, 0))

    def test_export_rejects_fallback_and_mixed_applied_traces(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_fixture(root / 'sample', layers=(2,), steps=2)
            export(export_parser().parse_args(
                [str(root / 'sample'), '--out-dir', str(root / 'e0'),
                 '--segment-bytes', '262144', '--declared-stride-tokens', str(STRIDE)]))
            pages = 3 * STRIDE * 4096 // 16384
            mapping = self.mapping(pages)
            plan = {}
            for row, cached, digest in baseline_states(root / 'e0', 4096):
                selected, swaps, _, _ = select(set(row['native']), row['candidates'], mapping,
                                               row['current_token'], 8, cached=cached)
                plan[row['step_id']] = (sorted(selected),
                                        [[o, n] for o, n in swaps], digest)
            for path in (root / 'sample').glob('*.json'):
                raw = json.loads(path.read_text())
                selected, swaps, digest = plan[raw['seq_lens_key'][0] - 10000]
                raw.update(selected=selected, swaps=swaps, cache_mode='shared', cache_capacity=4096,
                           cache_state_sha256=digest, layer_rank=2, stride_tokens=STRIDE)
                path.write_text(json.dumps(raw))
            # One step fell back to native: the whole trace must be refused.
            fallback = next((root / 'sample').glob('model_layers_2_s10001.json'))
            raw = json.loads(fallback.read_text())
            raw['overridden'] = False
            fallback.write_text(json.dumps(raw))
            with self.assertRaisesRegex(ValueError, 'override fell back|mixed'):
                export(export_parser().parse_args(
                    [str(root / 'sample'), '--out-dir', str(root / 'e4'), '--segment-bytes', '262144']))


if __name__ == '__main__':
    unittest.main()
