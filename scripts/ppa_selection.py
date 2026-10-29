"""Shared PPA-aware selection logic: one implementation, two consumers.

Consumed by scripts/mqsim_ppa_oracle.py (offline timing replay) and by
scripts/dsa_hook/dsa_dump_hook.py copied into the vllm-ascend container
(online override, second-generation traces). Standard library only, so the
module can be dropped next to the inference hook without extra dependencies.

Moved verbatim from mqsim_ppa_oracle.py (DenseMapping/load_mapping/
resources/pressure/objective/select) and simulate_hbm_candidates.py (BatchLRU);
existing tests import through the old modules and must keep passing unchanged.
"""
from array import array
from collections import Counter, OrderedDict
import csv
import hashlib

PAGE = 16384
SLOT = 4096


class BatchLRU:
    def __init__(self, capacity):
        self.capacity = capacity
        self.rows = OrderedDict()

    def commit(self, selected):
        selected = set(selected)
        if len(selected) > self.capacity:
            raise ValueError("Cache cannot hold the selected attention batch")
        # Protect all selected hits while allocating misses.
        for token in selected:
            self.rows.pop(token, None)
        while len(self.rows) + len(selected) > self.capacity:
            self.rows.popitem(last=False)
        for token in sorted(selected):
            self.rows[token] = None


def lru_digest(cache):
    """Hash of LRU membership AND order; the online hook records the same value."""
    return hashlib.sha256(array('Q', tuple(cache.rows)).tobytes()).hexdigest()


class DenseMapping:
    """Compact fixed-width mapping: 64 bytes/page instead of millions of dicts."""
    columns = ('lpa','ppa','channel','chip','die','plane','block','page')

    def __init__(self):
        self.data = array('Q')

    def __len__(self):
        return len(self.data)//8

    def __getitem__(self, lpa):
        if lpa < 0 or lpa >= len(self):
            raise KeyError(lpa)
        return dict(zip(self.columns, self.data[lpa*8:(lpa+1)*8]))


def load_mapping(path):
    result = DenseMapping()
    seen = set()
    with path.open() as f:
        for r in csv.DictReader(f):
            values = [int(r[k]) for k in result.columns]
            if values[0] != len(result) or values[1] in seen or min(values)<0:
                raise ValueError('Non-contiguous LPA or aliased/invalid PPA mapping')
            seen.add(values[1]); result.data.extend(values)
    return result


def resources(row):
    full = (row['channel'], row['chip'], row['die'], row['plane'])
    return tuple(full[:i] for i in range(1, 5))


def pressure(ids, info):
    result=Counter()
    for token in ids:
        result.update(resources(info[token]))
    return result


def objective(loads):
    # Static concentration proxy, not a NAND timing model. MQSim measures latency.
    return sum(v*v for v in loads.values())


def select(native, candidate_rows, mapping, current, budget, protect_top=0, cached=()):
    """Preserve all native hits; allow only scored Miss -> extra Miss swaps."""
    selected = set(native)
    resident = set(cached) | {current}
    scored = sorted((r for r in candidate_rows if r['score'] is not None),
                    key=lambda r: (-r['score'], r['token']))
    rank = {r['token']:i for i,r in enumerate(scored)}
    score = {r['token']:r['score'] for r in scored}
    info = {r['token']:mapping[r['offset_bytes']//PAGE] for r in candidate_rows}
    available = {r['token'] for r in scored} - selected - resident
    removable = {t for t in native if t not in resident and t in rank and rank[t] >= protect_top}
    loads = pressure(selected-resident, info)
    initial = objective(loads)
    exchanges=[]
    # Group identical resources: removing the worst-score native and adding the
    # best-score extra for that group preserves a deterministic quality preference.
    for _ in range(budget):
        remove_groups={}; add_groups={}
        for t in removable:
            key=resources(info[t])[-1]
            if key not in remove_groups or (score[t],-t) < (score[remove_groups[key]],-remove_groups[key]):
                remove_groups[key]=t
        for t in available:
            key=resources(info[t])[-1]
            if key not in add_groups or (-score[t],t) < (-score[add_groups[key]],add_groups[key]):
                add_groups[key]=t
        removals=sorted(remove_groups.values(), key=lambda t:(-sum(2*loads[k]-1 for k in resources(info[t])),score[t],t))[:16]
        additions=sorted(add_groups.values(), key=lambda t:(sum(2*loads[k]+1 for k in resources(info[t])),-score[t],t))[:16]
        best=None
        for old in removals:
            rk=resources(info[old])
            for new in additions:
                ak=resources(info[new])
                change=Counter(ak)
                change.subtract(rk)
                delta=sum(2*loads[k]*n+n*n for k,n in change.items())
                key=(delta,score[old]-score[new],old,new)
                if delta < 0 and (best is None or key < best[0]):
                    best=(key,old,new,change)
        if best is None:
            break
        _,old,new,change=best
        selected.remove(old); selected.add(new)
        removable.remove(old); available.remove(new)
        loads.update(change)
        exchanges.append((old,new))
    return selected, exchanges, initial, objective(loads)


def declared_stride_offsets(layer_ids, stride_tokens, slot=SLOT):
    """Online layout contract v2: every layer occupies stride_tokens*slot bytes.

    Unlike export_ppa_candidates' cumulative layout (bases from each layer's
    final context, no inter-layer padding), this is fixed before the run starts
    from a declared maximum context, so an online run that stops early (or
    diverges) still addresses exactly the same bytes the prepare run wrote.
    Rank is the position in ascending numeric layer-id order; contiguous ids
    make rank == numeric id.
    """
    if stride_tokens <= 0:
        raise ValueError('stride must be positive')
    return {layer: rank*stride_tokens*slot for rank, layer in enumerate(sorted(layer_ids))}


def token_offset(base_bytes, token, slot=SLOT):
    return base_bytes + token*slot
