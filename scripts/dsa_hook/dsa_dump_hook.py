"""Opt-in Top-2400 FP32 reference-indexer dump for vllm-ascend sfa_v1 (DSA).

Per decode step and layer, capture two things side by side:
  1. native indices: the top-2048 the fused lightning indexer actually fed
     into npu_sparse_flash_attention;
  2. reference candidates: an independent FP32 recomputation of the CANN
     lightning-indexer score  I_j = sum_h w_h * ReLU(q_h . k_j)  over the
     full context, keeping the top-2400 token ids and scores.

The reference reuses exactly the post-RoPE q_li and weights_proj output the
fused kernel saw (stashed inside indexer_select_post_process, before the
sparse-c8 quantization reassigns them), so ranking divergence against the
native set comes only from FP32 accumulation vs fused kernel internals --
the divergence the offline pipeline already tracks as native_covered overlap.

Output, one file per layer/step, schema of simulate_hbm_candidates.load_record
(consumed unchanged by scripts/export_ppa_candidates.py):
  <DSA_DUMP_DIR>/<layer_name>_s<seq_len>.pt
    seq_lens_key=[S], seq_lens_query=[1], indices (native, sorted),
    candidate_indices, candidate_scores (FP32, descending),
    has_reference_candidates=True, layer=<layer_name>
plus one append-only manifest.jsonl line per dump and a _summary.json at exit
(counters, per-layer last seq, config).

Override mode (DSA_OVERRIDE_ENABLE=1) additionally applies the PPA-aware
selection ONLINE, producing second-generation traces: after computing the
reference ranking, the hook runs the shared scripts/ppa_selection.py select()
against the static prepare-run PPA mapping and an in-process per-layer
BatchLRU advanced with native Top-2048 (shared-baseline semantics), replaces
the index tensor handed to npu_sparse_flash_attention, and records the applied
selection in the dump (selected/swaps/cache_state_sha256/layer_rank/
stride_tokens). maybe_dump() returns the (possibly replaced) index tensor;
the sfa_v1 patch assigns the return value. ppa_selection.py must be copied
next to this module in the container.

Opt-in: DSA_DUMP_ENABLE=1. Only DSA_DUMP_RANK (default 0) does any work; the
other TP ranks return before touching tensors. Requires the replicated
(non-sharded) indexer that plain TP deployments use (all heads on every rank;
that is also what makes per-rank native top-k identical). Single-request
decode steps only: prefill, multi-request batches, DSA-CP and sparse-c8 paths
are skipped and counted. Run with --enforce-eager so decode batches are
unpadded. The dump rank synchronizes twice per layer-step; expect slower
generation while collecting.
"""
import atexit
from array import array
import hashlib
import json
import math
import os
from pathlib import Path
import re
import time

import torch

LAYER_ID_RE = re.compile(r"(?:^|\.)layers\.(\d+)(?:\.|$)")
SAMPLE_RE = re.compile(r"[^A-Za-z0-9._-]+")

_state = {
    'cfg': None,
    'stash': None,
    'counters': {},
    'last_seq': {},
    'warned': set(),
    'manifest': None,
    'ready': False,
    'sample': '_',
}


def _config():
    if _state['cfg'] is None:
        env = os.environ.get
        _state['cfg'] = dict(
            enable=env('DSA_DUMP_ENABLE') == '1',
            dump_dir=env('DSA_DUMP_DIR', '').strip(),
            rank=int(env('DSA_DUMP_RANK', '0')),
            ref_topk=int(env('DSA_DUMP_REF_TOPK', '2400')),
            native_topk=int(env('DSA_DUMP_NATIVE_TOPK', '2048')),
            min_seq=int(env('DSA_DUMP_MIN_SEQ', '2049')),
            max_steps=int(env('DSA_DUMP_MAX_STEPS_PER_LAYER', '0')),
            scale=float(env('DSA_DUMP_SCALE', '1.0')),
            use_relu=env('DSA_DUMP_INDEXER_RELU', '1') != '0',
            log_every=int(env('DSA_DUMP_LOG_EVERY', '100')),
        )
    return _state['cfg']


def _bump(key):
    _state['counters'][key] = _state['counters'].get(key, 0) + 1


def _warn_once(key, message):
    if key not in _state['warned']:
        _state['warned'].add(key)
        print(f'[IndexerTrace] {message}', flush=True)


def _marker_sample(root):
    """Sample id from the current_sample marker (doc 17 convention).

    Datasets run many examples through one server; each example must land in
    its own dump directory with its own cold LRU, or seq continuity and the
    offline one-sample-per-directory contract break. Marker absent -> '_'
    (dumps stay in the root directory, single-sample behavior).
    """
    try:
        text = (root / 'current_sample').read_text().strip()
    except OSError:
        return '_'
    return SAMPLE_RE.sub('_', text)[:64] if text else '_'


def _rotate_sample(sample):
    """Switch the active sample: fresh manifest, per-layer seq chain and LRU."""
    if _state['manifest'] is not None:
        _state['manifest'].close()
        _state['manifest'] = None
    _state['last_seq'] = {}
    _override['lrus'] = {}
    for key in [k for k in _state['counters'] if k.startswith('dumps:')]:
        del _state['counters'][key]
    _state['sample'] = sample
    _state['ready'] = False
    print(f'[IndexerTrace] sample -> {sample}', flush=True)


# ---------------- online PPA-aware override (second-generation traces) ------

_override = {'cfg': None, 'mapping': None, 'mapping_sha': None, 'lrus': {}}


def _override_config():
    """Parse DSA_OVERRIDE_* once; None when disabled or invalid (warned)."""
    if _override['cfg'] is None:
        env = os.environ
        if env.get('DSA_OVERRIDE_ENABLE') != '1':
            _override['cfg'] = False
            return None
        try:
            import ppa_selection
        except ImportError:
            _warn_once('ovr-import', 'DSA_OVERRIDE_ENABLE=1 but ppa_selection.py is not '
                        'importable next to dsa_dump_hook; override disabled')
            _override['cfg'] = False
            return None
        try:
            if not _config()['enable']:
                raise ValueError('DSA_OVERRIDE_ENABLE=1 requires DSA_DUMP_ENABLE=1 '
                                 '(the dump is the second-generation trace)')
            ranks = (json.loads(Path(env['DSA_OVERRIDE_LAYER_RANKS']).read_text())
                     if env.get('DSA_OVERRIDE_LAYER_RANKS') else None)
            cfg = dict(capacity=int(env.get('DSA_OVERRIDE_CAPACITY', '4096')),
                       budget=int(env.get('DSA_OVERRIDE_BUDGET', '128')),
                       protect_top=int(env.get('DSA_OVERRIDE_PROTECT_TOP', '0')),
                       stride=int(env.get('DSA_OVERRIDE_STRIDE_TOKENS', '0')),
                       mapping_path=env.get('DSA_OVERRIDE_MAPPING', '').strip(),
                       cache=env.get('DSA_OVERRIDE_CACHE', 'shared'),
                       verify_ranks=env.get('DSA_OVERRIDE_VERIFY_RANKS', '1') == '1',
                       rank_map=ranks)
            if (cfg['capacity'] < 2048 or not 0 <= cfg['budget'] <= 352
                    or cfg['protect_top'] < 0 or cfg['stride'] <= 0
                    or not cfg['mapping_path'] or cfg['cache'] != 'shared'):
                raise ValueError('invalid DSA_OVERRIDE_* combination')
        except (KeyError, ValueError) as exc:
            _warn_once('ovr-cfg', f'override disabled: {exc}')
            _override['cfg'] = False
            return None
        _override['cfg'] = cfg
    return _override['cfg'] or None


def _override_mapping(cfg):
    if _override['mapping'] is None:
        path = Path(cfg['mapping_path'])
        started = time.time()
        from ppa_selection import load_mapping
        _override['mapping'] = load_mapping(path)
        _override['mapping_sha'] = hashlib.sha256(path.read_bytes()).hexdigest()
        print(f'[IndexerTrace] override mapping loaded: {len(_override["mapping"])} pages, '
              f'sha256={_override["mapping_sha"][:16]}..., {time.time()-started:.1f}s', flush=True)
    return _override['mapping']


def _apply_override(impl, layer_name, seq, native, cand_ids, cand_scores):
    """Run the shared select() online on THIS rank; deterministic inputs make
    the result identical on every rank. Returns (selected, swaps, lru_digest,
    layer_rank, override_cfg) or None when the step falls back to native."""
    cfg = _override_config()
    if cfg is None:
        return None
    match = LAYER_ID_RE.search(str(layer_name))
    if match is None:
        _warn_once(f'ovr-layer:{layer_name}', f'cannot parse layers.N from {layer_name}; '
                    'override falls back to native for it')
        return None
    layer_id = int(match.group(1))
    rank = cfg['rank_map'][str(layer_id)] if cfg['rank_map'] else layer_id
    from ppa_selection import BatchLRU, SLOT, select, lru_digest
    mapping = _override_mapping(cfg)
    base = rank * cfg['stride'] * SLOT
    if (base + (seq - 1) * SLOT) // 16384 >= len(mapping):
        _warn_once('ovr-maprange', f'seq {seq} exceeds prepared mapping '
                    f'({len(mapping)} pages); override falls back to native')
        return None
    lru = _override['lrus'].setdefault(layer_name, BatchLRU(cfg['capacity']))
    scores = dict(zip(cand_ids, cand_scores))
    # Union the native set in: select() looks up info[] for every selected
    # native token, and natives absent from the reference ranking get score
    # None (protected), mirroring export_ppa_candidates.
    rows = [dict(token=t, score=scores.get(t), offset_bytes=base + t * SLOT)
            for t in sorted(set(native) | set(cand_ids))]
    selected, swaps, _, _ = select(set(native), rows, mapping, current=seq - 1,
                                   budget=cfg['budget'], protect_top=cfg['protect_top'],
                                   cached=set(lru.rows))
    if len(selected) != len(set(native)):
        _warn_once('ovr-invariant', 'select() changed the selection size; '
                    'override falls back to native')
        return None
    digest = lru_digest(lru)
    lru.commit(native)  # shared-baseline semantics: cache advances with native only
    return selected, swaps, digest, rank, cfg


def stash_indexer_inputs(impl, q_li, weights):
    """Called from indexer_select_post_process right after RoPE, pre-quant."""
    cfg = _config()
    if not cfg['enable']:
        return
    # Override runs on every TP rank (selections must be identical), so the
    # stash is kept rank-wide whenever override mode is active.
    if impl.tp_rank != cfg['rank'] and _override_config() is None:
        return
    if impl.use_sparse_c8_indexer:
        _warn_once('c8', 'sparse-c8 indexer path is not dumpable '
                         '(hadamard+int8 k cache); skipping all dumps')
        return
    _state['stash'] = (
        q_li.detach().to(torch.float32),
        weights.detach().to(torch.float32),
    )


def _gather_indexer_keys(kv_cache, block_table, seq, head_dim):
    """Full-context FP32 indexer keys from the paged k_li cache (kv_cache[2])."""
    cache = kv_cache[2]
    if cache.dim() != 4 or cache.shape[2] != 1 or cache.shape[3] != head_dim:
        _warn_once('cache', f'unexpected indexer cache shape {tuple(cache.shape)}')
        return None
    block_size = cache.shape[1]
    n_blocks = (seq + block_size - 1) // block_size
    if block_table.shape[0] < 1 or block_table.shape[1] < n_blocks:
        _warn_once('block_table', f'block_table too small for seq={seq}')
        return None
    blocks = block_table[0, :n_blocks].to(torch.int64).reshape(-1)
    offsets = torch.arange(block_size, device=cache.device, dtype=torch.int64)
    positions = blocks[:, None] * block_size + offsets[None, :]
    rows = cache.reshape(-1, head_dim)[positions.reshape(-1)][:seq]
    return rows.to(torch.float32)


def _verify_rank_consistency(reference_tensor, sorted_ids):
    """Fail loudly if any TP rank computed a different override.

    DSA selects ONE token set per sequence shared by all heads, and with plain
    TP every rank runs attention over its own copy of that set: a silent
    disagreement between ranks corrupts the model. The replicated indexer
    gives every rank identical inputs, so identical selections are expected;
    a digest mismatch raises instead of returning inconsistent tensors.
    """
    try:
        import torch.distributed as dist
    except ImportError:
        _warn_once('ovr-dist', 'torch.distributed unavailable; cross-rank check skipped')
        return
    if not dist.is_initialized():
        return
    digest = int.from_bytes(hashlib.sha256(
        array('q', sorted_ids).tobytes()).digest()[:8], 'little') & 0x7FFFFFFFFFFFFFFF
    low = torch.tensor([digest], dtype=torch.int64, device=reference_tensor.device)
    high = low.clone()
    dist.all_reduce(low, op=dist.ReduceOp.MIN)
    dist.all_reduce(high, op=dist.ReduceOp.MAX)
    if int(low[0]) != int(high[0]):
        raise RuntimeError(f'[IndexerTrace] TP ranks computed different overrides '
                           f'(digest {int(low[0])} vs {int(high[0])}); refusing to mix '
                           'inconsistent attention sets')


def maybe_dump(impl, layer_name, topk_indices, attn_metadata, kv_cache,
               actual_seq_lengths_query, actual_seq_lengths_key):
    """Called from AscendSFAImpl.forward right after the fused top-k.

    Returns the index tensor to hand to npu_sparse_flash_attention: the
    original one, or the PPA-aware replacement when override mode is active.
    Dump-only mode runs entirely on the dump rank; override mode runs the
    same deterministic computation on EVERY rank (identical replicated
    inputs -> identical selections) and only the dump I/O stays rank-gated.
    """
    cfg = _config()
    ocfg = _override_config()
    dump_rank = cfg['enable'] and impl.tp_rank == cfg['rank']
    if ocfg is None and not dump_rank:
        return topk_indices
    _bump('forward_calls')
    state = getattr(attn_metadata.attn_state, 'name', str(attn_metadata.attn_state))
    if state != 'DecodeOnly':
        _bump('skip_state')
        return topk_indices
    if impl.enable_dsa_cp:
        _warn_once('cp', 'DSA-CP path is not supported by the dump hook; skipping')
        _bump('skip_cp')
        return topk_indices
    if kv_cache is None or len(kv_cache) < 3:
        _bump('skip_kv_cache')
        return topk_indices
    # DecodeOnly with one request means exactly one query token; only the
    # tensor shape is checked here (no device sync).
    if actual_seq_lengths_query.numel() != 1 or actual_seq_lengths_key.numel() != 1:
        _bump('skip_multi_request')
        return topk_indices
    seq = int(attn_metadata.seq_lens_cpu[0])
    if seq < cfg['min_seq']:
        _bump('skip_short_seq')
        return topk_indices
    stash = _state['stash']
    if stash is None:
        _bump('skip_no_stash')
        return topk_indices
    q_li, weights = stash
    n_head, head_dim = impl.n_head, impl.head_dim
    if q_li.dim() != 3 or tuple(q_li.shape[1:]) != (n_head, head_dim) or q_li.shape[0] != 1:
        _warn_once('heads', f'indexer q_li has {tuple(q_li.shape)}, expected '
                            f'[1,{n_head},{head_dim}] (replicated indexer); skipping')
        _bump('skip_head_shape')
        return topk_indices
    if weights.numel() != n_head:
        _warn_once('weights', f'indexer weights numel={weights.numel()}, '
                              f'expected {n_head}; skipping')
        _bump('skip_weight_shape')
        return topk_indices
    last = _state['last_seq'].get(layer_name)
    if last is not None and seq <= last:
        _bump('skip_replayed_step')
        return topk_indices
    if last is not None and seq != last + 1:
        _warn_once(f'gap:{layer_name}', f'{layer_name}: seq jumped {last} -> {seq}; '
                   'earlier steps were missed (check counters); dumps continue')
        _bump('seq_gaps')
    if cfg['max_steps'] and _state['counters'].get(f'dumps:{layer_name}', 0) >= cfg['max_steps']:
        _bump('skip_step_limit')
        return topk_indices
    native = topk_indices.detach().view(-1).to(torch.int64).cpu().tolist()
    expected = min(cfg['native_topk'], seq)
    if (len(native) != expected or len(set(native)) != len(native)
            or any(not 0 <= t < seq for t in native)):
        _warn_once('native', f'{layer_name} s={seq}: fused top-k returned {len(native)} '
                             f'indices, expected {expected}; skipping this step')
        _bump('skip_native_invalid')
        return topk_indices
    started = time.time()
    keys = _gather_indexer_keys(kv_cache, attn_metadata.block_table, seq, head_dim)
    if keys is None:
        _bump('skip_key_gather')
        return topk_indices
    sim = q_li[0] @ keys.transpose(0, 1)                     # [H, S] in FP32
    if cfg['use_relu']:
        sim = torch.relu(sim)
    scores = (weights.reshape(-1)[:, None] * sim).sum(dim=0) * cfg['scale']
    keep = min(cfg['ref_topk'], seq)
    top_scores, top_ids = torch.topk(scores, keep)
    cand_scores = top_scores.cpu().tolist()
    cand_ids = top_ids.cpu().tolist()
    if any(not math.isfinite(s) for s in cand_scores):
        _warn_once('finite', f'{layer_name} s={seq}: non-finite reference scores; skipping')
        _bump('skip_nonfinite')
        return topk_indices
    # Sample rotation must happen at the same point on EVERY rank (it resets
    # the override LRU), so it is checked before the rank-gated dump path.
    root = Path(cfg['dump_dir'])
    sample = _marker_sample(root)
    if sample != _state['sample'] and (ocfg is not None or dump_rank):
        _rotate_sample(sample)
    override = (_apply_override(impl, layer_name, seq, native, cand_ids, cand_scores)
                if ocfg is not None else None)
    result_indices = topk_indices
    if override is not None:
        # budget=0 or a perfectly balanced step keeps the native set: then the
        # tensor is returned unchanged and the output stays bit-identical.
        selected, swaps, digest, rank, ocfg = override
        if selected != set(native):
            result_indices = torch.tensor(sorted(selected), dtype=topk_indices.dtype,
                                          device=topk_indices.device).view(topk_indices.shape)
        if ocfg['verify_ranks']:
            _verify_rank_consistency(topk_indices, sorted(selected))
    if not dump_rank:
        return result_indices
    out_dir = root if sample == '_' else root / sample
    record = dict(seq_lens_key=[seq], seq_lens_query=[1],
                  indices=sorted(native), candidate_indices=cand_ids,
                  candidate_scores=cand_scores, has_reference_candidates=True,
                  layer=str(layer_name))
    if ocfg is not None:
        if override is not None:
            record.update(selected=sorted(selected), swaps=[[o, n] for o, n in swaps],
                          overridden=True, cache_mode='shared',
                          cache_capacity=ocfg['capacity'], cache_state_sha256=digest,
                          layer_rank=rank, stride_tokens=ocfg['stride'])
        else:
            # Override active but this step fell back to native (identically on
            # every rank); the offline applied replay must refuse such traces.
            record.update(overridden=False)
    if not _state['ready']:
        if not cfg['dump_dir']:
            _warn_once('dir', 'DSA_DUMP_ENABLE=1 but DSA_DUMP_DIR is empty; skipping')
            _bump('skip_no_dir')
            return result_indices
        out_dir.mkdir(parents=True, exist_ok=True)
        print(f'[IndexerTrace] hook ready: dir={out_dir} rank={cfg["rank"]} '
              f'ref_topk={cfg["ref_topk"]} min_seq={cfg["min_seq"]}', flush=True)
        _state['ready'] = True
        if ocfg is not None:
            (out_dir / '_override_config.json').write_text(json.dumps(dict(
                mapping_sha256=_override['mapping_sha'], capacity=ocfg['capacity'],
                budget=ocfg['budget'], protect_top=ocfg['protect_top'],
                stride_tokens=ocfg['stride'], cache_mode='shared',
                rank_map=ocfg['rank_map'], rank_source='record.layer_rank',
            ), indent=2) + '\n')
    path = out_dir / f'{layer_name}_s{seq}.pt'
    tmp = path.with_name(path.name + '.tmp')
    torch.save(record, tmp)
    os.replace(tmp, path)
    overlap = len(set(native) & set(cand_ids))
    _manifest_line(out_dir, dict(layer=str(layer_name), seq=seq, native=len(native),
                                 candidates=keep, native_in_candidates=overlap,
                                 overridden=bool(override), swaps=len(override[1]) if override else 0,
                                 seconds=round(time.time() - started, 4)))
    if ocfg is not None:
        _bump('override_applied' if override else 'override_fallback')
    _bump('dumps')
    _bump(f'dumps:{layer_name}')
    total = _state['counters']['dumps']
    if total % cfg['log_every'] == 0:
        visible = {k: v for k, v in _state['counters'].items() if ':' not in k}
        print(f'[IndexerTrace] dumps={total} {json.dumps(visible)}', flush=True)
    return result_indices


def _manifest_line(out_dir, entry):
    if _state['manifest'] is None:
        _state['manifest'] = (out_dir / 'manifest.jsonl').open('a')
    _state['manifest'].write(json.dumps(entry) + '\n')
    _state['manifest'].flush()


def _write_summary():
    cfg = _state['cfg']
    if cfg is None or not _state['ready']:
        return
    if _state['manifest'] is not None:
        _state['manifest'].close()
    out_dir = Path(cfg['dump_dir'])
    counters = dict(_state['counters'])
    summary = dict(finished_at=time.time(), config=cfg, counters=counters,
                   per_layer_dumps={k[6:]: v for k, v in counters.items()
                                    if k.startswith('dumps:')},
                   last_seq=_state['last_seq'], sample=_state['sample'],
                   override_mapping_sha256=_override['mapping_sha'])
    tmp = out_dir / '_summary.json.tmp'
    tmp.write_text(json.dumps(summary, indent=2, default=str) + '\n')
    os.replace(tmp, out_dir / '_summary.json')
    print(f'[IndexerTrace] summary: {out_dir / "_summary.json"} '
          f'dumps={counters.get("dumps", 0)}', flush=True)


atexit.register(_write_summary)
