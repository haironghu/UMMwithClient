#!/usr/bin/env python3
"""PPA-aware fixed-trajectory replay through patched official MQSim NVMe/FTL/NAND.

Single synthetic SSD; full snapshot writes, then layer barriers. All policies
share the native baseline's per-layer batch-LRU trajectory. Only cache misses
can be replaced, and only by extra candidates that also miss. Report simulator
latency separately from Python selection cost. Not a model quality/TPOT test.
"""
import argparse
from array import array
from collections import Counter
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import random
import statistics
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET

from setup_mqsim_oracle import PATCH, REVISION, ROOT
from simulate_hbm_candidates import BatchLRU

PAGE = 16384
SLOT = 4096


def sha(path):
    d = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1 << 20), b''):
            d.update(data)
    return d.hexdigest()


def records(directory):
    with (directory/'candidates.jsonl').open() as f:
        for line in f:
            yield json.loads(line)


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


def baseline_states(directory, capacity):
    """Yield immutable pre-step state; advance ONLY with native Top-2048.

    Rebuilding this trajectory for each case is deterministic. The digest
    includes LRU order, not just membership; cross-case checks enforce equality.
    capacity=0 is an explicit no-HBM control, retained for old smoke experiments.
    """
    caches = {}
    for row in records(directory):
        if row['layer_id'] not in caches:
            caches[row['layer_id']] = BatchLRU(capacity)
        cache = caches[row['layer_id']]
        state = tuple(cache.rows)
        digest = hashlib.sha256(array('Q', state).tobytes()).hexdigest()
        if capacity:
            cache.commit(row['native'])
        yield row, frozenset(state), digest


def percentile(values, p):
    if not values: return 0.0
    a=sorted(values); pos=(len(a)-1)*p; i=int(pos)
    return a[i]+(a[min(i+1,len(a)-1)]-a[i])*(pos-i)


def configure(source, path, snapshot_bytes, user_config=None):
    doc=ET.parse(user_config or source/'ssdconfig.xml')
    def setv(k,v):
        node=doc.find('.//'+k)
        if node is None: raise ValueError(f'Missing MQSim parameter: {k}')
        node.text=str(v)
    if user_config is None:
        # Small reference SSD, NOT a calibrated model of the user's actual drive.
        for k,v in dict(Flash_Channel_Count=4,Chip_No_Per_Channel=1,Die_No_Per_Chip=2,
                        Plane_No_Per_Die=2,Page_Capacity=PAGE,Page_No_Per_Block=128,
                        Plane_Allocation_Scheme='CWDP').items(): setv(k,v)
        needed=math.ceil((snapshot_bytes/PAGE)/(16*128*.60))
        setv('Block_No_Per_Plane',max(32,math.ceil(needed/32)*32))
    for k,v in dict(Enabled_Preconditioning='false',Ideal_Mapping_Table='true',
                    Dynamic_Wearleveling_Enabled='false',Static_Wearleveling_Enabled='false',
                    Transaction_Scheduling_Policy='OUT_OF_ORDER').items(): setv(k,v)
    def get(k): return int(doc.find('.//'+k).text)
    if get('Page_Capacity') != PAGE:
        raise ValueError('First implementation requires 16 KiB NAND pages')
    count=math.prod(get(k) for k in ('Flash_Channel_Count','Chip_No_Per_Channel','Die_No_Per_Chip','Plane_No_Per_Die','Block_No_Per_Plane','Page_No_Per_Block'))
    if snapshot_bytes > count*PAGE*.60:
        raise ValueError('Snapshot exceeds 60% physical capacity; increase blocks to keep GC outside this experiment')
    if doc.find('.//HostInterface_Type').text != 'NVME' or doc.find('.//Address_Mapping').text != 'PAGE_LEVEL':
        raise ValueError('Only NVMe with page-level FTL is supported')
    doc.write(path,encoding='utf-8',xml_declaration=True)
    return doc


def write_snapshot(stream, snapshot_bytes, segment):
    for offset in range(0,snapshot_bytes,segment):
        stream.write(f'0 0 {offset//512} {segment//512} 0\n')


def run_sim(source, config, directory, qd, snapshot_bytes, segment):
    directory.mkdir(exist_ok=True)
    work=ET.Element('MQSim_IO_Scenarios');scenario=ET.SubElement(work,'IO_Scenario')
    flow=ET.SubElement(scenario,'IO_Flow_Parameter_Set_Trace_Based')
    doc=ET.parse(config)
    params={'Priority_Class':'HIGH','Device_Level_Data_Caching_Mode':'TURNED_OFF',
            'Initial_Occupancy_Percentage':0,'File_Path':str(directory/'input.trace'),
            'Percentage_To_Be_Executed':100,'Relay_Count':1,'Time_Unit':'NANOSECOND'}
    for key,param in [('Channel_IDs','Flash_Channel_Count'),('Chip_IDs','Chip_No_Per_Channel'),
                      ('Die_IDs','Die_No_Per_Chip'),('Plane_IDs','Plane_No_Per_Die')]:
        params[key]=','.join(str(i) for i in range(int(doc.find('.//'+param).text)))
    for k,v in params.items(): ET.SubElement(flow,k).text=str(v)
    ET.ElementTree(work).write(directory/'workload.xml',encoding='utf-8',xml_declaration=True)
    env=dict(os.environ,DSA_ORACLE_QD=str(qd),DSA_ORACLE_BATCHES=str(directory/'batches.csv'),
             DSA_ORACLE_MAPPING=str(directory/'ppa.csv'),DSA_ORACLE_NAND=str(directory/'nand.csv'))
    print(f'MQSim {directory.name}',flush=True)
    with (directory/'mqsim.log').open('w') as log:
        result=subprocess.run([str(source/'MQSim'),'-i',str(config),'-w',str(directory/'workload.xml')],
                              cwd=directory,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'MQSim failed ({result.returncode}); inspect {directory/"mqsim.log"}')
    with (directory/'batches.csv').open() as f:
        batches=[{k:int(v) for k,v in r.items()} for r in csv.DictReader(f)]
    if not batches or batches[0]['batch']!=0 or batches[0]['requests']!=snapshot_bytes//segment:
        raise ValueError('Snapshot completion missing')
    # Source USERIO=0; reject unmodeled GC/mapping/cache traffic in this read-only experiment.
    written_bytes = 0
    with (directory/'nand.csv').open() as f:
        for row in csv.DictReader(f):
            if int(row['source'])!=0 or int(row['type']) not in (0,1):
                raise ValueError('Unexpected GC/mapping/erase traffic; run not comparable')
            if int(row['type'])==1: written_bytes+=int(row['bytes'])
    if written_bytes != snapshot_bytes:
        raise ValueError('Snapshot write completion/byte count mismatch')
    return batches,directory/'ppa.csv',directory/'nand.csv'


def read_nand(path):
    with path.open() as f:
        for row in csv.DictReader(f):
            if int(row['type'])==0:
                yield {k:int(v) for k,v in row.items()}


def validate_export(directory):
    m=json.loads((directory/'manifest.json').read_text())
    if m.get('status')!='complete' or m.get('device_count')!=1 or m.get('slot_bytes')!=SLOT:
        raise ValueError('Need a complete single-SSD candidate export')
    if sha(directory/'candidates.jsonl')!=m['candidates_sha256']:
        raise ValueError('Candidate trace checksum mismatch')
    if m['segment_bytes']%PAGE or m['segment_bytes']//512 > 65535:
        raise ValueError('Write segment must be 16 KiB aligned and fit one NVMe command')
    if m['snapshot_bytes']%m['segment_bytes']:
        raise ValueError('Snapshot must be padded to write segment')
    previous=None; count=0
    geometry={r['layer_id']:r for r in m['layers']}
    for r in records(directory):
        key=(r['step_id'],r['layer_id'])
        if previous is not None and key<=previous: raise ValueError('Candidates must be ordered step/layer')
        previous=key; count+=1
        g=geometry[r['layer_id']]
        native=r['native']; candidates=r['candidates']; ids={x['token'] for x in candidates}
        if (len(native)!=min(2048,r['context_length']) or len(set(native))!=len(native)
                or not set(native)<=ids or len(ids)!=len(candidates)):
            raise ValueError('Invalid native or candidate set')
        scored=[x for x in candidates if x['score'] is not None]
        if len(scored)!=min(2400,r['context_length']) or any(not math.isfinite(x['score']) for x in scored):
            raise ValueError('Invalid reference scores')
        if r['context_length']<1 or r['current_token']!=r['context_length']-1 or r['context_length']>g['context_length']:
            raise ValueError('Invalid context geometry')
        for x in candidates:
            if not 0<=x['token']<r['context_length'] or x['device']!=0 or x['offset_bytes']!=g['base_bytes']+x['token']*SLOT:
                raise ValueError('Invalid token address')
    if count!=m['records'] or count==0: raise ValueError('Record count mismatch')
    return m


def selections_with_baseline(directory, mapping, capacity, strategy, budget, protect_top, seed, warmup_steps):
    """Plan one case against the unchanged native-cache trajectory."""
    for index, (row, cached, cache_digest) in enumerate(baseline_states(directory, capacity), 1):
        native = set(row['native']); current = row['current_token']
        resident = cached | {current}
        native_misses = native - resident
        start = time.perf_counter()
        selected, swaps, before, _ = select(native, row['candidates'], mapping, current,
                                            budget, protect_top, cached=cached)
        if strategy == 'baseline':
            selected = native; swaps = []
        elif strategy == 'random_matched':
            ranked = sorted((x for x in row['candidates'] if x['score'] is not None),
                            key=lambda x: (-x['score'], x['token']))
            protected = {x['token'] for x in ranked[:protect_top]} | resident
            scored = {x['token'] for x in ranked}
            eligible = sorted((native & scored) - protected)
            extra = sorted(scored - native - resident)
            rng_seed = int.from_bytes(hashlib.sha256(f'{seed}:{index}:{budget}'.encode()).digest()[:8], 'little')
            rng = random.Random(rng_seed); n = len(swaps)
            old = rng.sample(eligible, n); new = rng.sample(extra, n)
            selected = (native - set(old)) | set(new); swaps = list(zip(old, new))
        decision_ms = (time.perf_counter() - start) * 1000
        info = {x['token']:mapping[x['offset_bytes']//PAGE] for x in row['candidates']}
        addresses = {x['token']:x['offset_bytes'] for x in row['candidates']}
        scores = {x['token']:x['score'] for x in row['candidates']}
        emitted = sorted(selected - resident)
        native_hits = native & cached
        fresh_hits = int(current in native and current not in cached)
        if (len(selected) != len(native) or (selected & resident) != (native & resident)
                or len(emitted) != len(native_misses)
                or any(old not in native_misses or new in resident or new in native for old, new in swaps)):
            raise ValueError('Miss-only replacement changed native hits/current-token availability/read volume')
        contract = hashlib.sha256(json.dumps([row['step_id'], row['layer_id'], cache_digest,
                      sorted(native), sorted(native_misses), len(native_hits), fresh_hits],
                      separators=(',', ':')).encode()).hexdigest()
        loads = pressure(emitted, info)
        m = dict(batch=index, step=row['step_id'], layer=row['layer_id'],
                 capacity_rows_per_layer=capacity, selected=len(selected), cache_hits=len(native_hits),
                 fresh_hits=fresh_hits, misses=len(emitted), requests=len(emitted),
                 cache_state_sha256=cache_digest, baseline_contract_sha256=contract,
                 warmup=row['step_id'] < warmup_steps, replacements=len(swaps),
                 score_loss=sum(scores[old]-scores[new] for old, new in swaps),
                 decision_ms=decision_ms, objective_before=before, objective_after=objective(loads),
                 unique_physical_pages=len({info[t]['ppa'] for t in emitted}),
                 peak_channel=max((v for k,v in loads.items() if len(k)==1), default=0),
                 peak_die=max((v for k,v in loads.items() if len(k)==3), default=0),
                 peak_plane=max((v for k,v in loads.items() if len(k)==4), default=0))
        audit = dict(**m, selected_indices=sorted(selected), miss_indices=emitted,
                     native_miss_indices=sorted(native_misses), swaps=swaps)
        yield m, audit, [addresses[token] for token in emitted]


def completed_contracts(case, result, manifest, warmup, mapsha):
    """Validate saved case metadata before skipping an expensive completed run."""
    if sha(case/'ppa.csv') != mapsha:
        raise ValueError(f'Resume mapping mismatch: {case}')
    if not (case/'selections.jsonl').is_file():
        raise ValueError(f'Resume selections missing: {case}')
    with (case/'layers.csv').open() as f:
        rows=list(csv.DictReader(f))
    if len(rows)!=manifest['records']:
        raise ValueError(f'Resume layer count mismatch: {case}')
    contracts=[]; measured=[]
    for index,row in enumerate(rows,1):
        is_warm=int(row['step'])<warmup
        if (int(row['batch'])!=index or int(row['capacity_rows_per_layer'])!=result['capacity_rows_per_layer']
                or row['warmup']!=str(is_warm)):
            raise ValueError(f'Resume layer metadata mismatch: {case}')
        contracts.append(row['baseline_contract_sha256'])
        if not is_warm: measured.append(row)
    expected=dict(measured_batches=len(measured),
                  zero_miss_batches=sum(int(r['requests'])==0 for r in measured))
    for key in ('selected','cache_hits','fresh_hits','misses','requests','replacements',
                'nand_read_transactions','nand_read_payload_bytes'):
        expected[key]=sum(int(r[key]) for r in measured)
    expected['baseline_cache_trace_sha256']=hashlib.sha256(''.join(contracts).encode()).hexdigest()
    if (any(result[k]!=v for k,v in expected.items()) or
            not math.isclose(result['read_seconds'],sum(int(r['elapsed_ns']) for r in measured)/1e9,
                             rel_tol=1e-12,abs_tol=1e-12)):
        raise ValueError(f'Resume summary/layers mismatch: {case}')
    return contracts


def save_report(out, report):
    """Atomically replace checkpoints so an interrupted write preserves the old file."""
    path=out/'summary.json'
    pending=out/'summary.json.tmp'
    pending.write_text(json.dumps(report,indent=2)+'\n'); pending.replace(path)
    if report['results']:
        pending=out/'comparison.csv.tmp'
        with pending.open('w') as f:
            writer=csv.DictWriter(f,fieldnames=list(report['results'][0]))
            writer.writeheader(); writer.writerows(report['results'])
        pending.replace(out/'comparison.csv')


def main(args):
    source=args.mqsim.resolve(); inputs=args.candidates.resolve(); out=args.out_dir.resolve()
    capacities=sorted(set(args.capacities))
    if (args.qd<1 or args.qd>65534 or any(b<0 or b>352 for b in args.budgets)
            or args.protect_top<0 or args.warmup_steps<0
            or any(c != 0 and c<2048 for c in capacities)):
        raise ValueError('Invalid QD/budget/protection/warmup/capacity (use 0 or >=2048 rows)')
    print(f'Validating candidates: {inputs}',flush=True)
    manifest=validate_export(inputs)
    print(f'Validated {manifest["records"]} layer/step records',flush=True)
    if not any(r['step_id']>=args.warmup_steps for r in records(inputs)):
        raise ValueError('Warmup excludes all records; use --warmup-steps 0 for short smoke runs')
    build=json.loads((source/'dsa-oracle-build.json').read_text())
    if (build['upstream_revision']!=REVISION or build['patch_sha256']!=sha(PATCH) or build['binary_sha256']!=sha(source/'MQSim')):
        raise ValueError('MQSim build mismatch; rerun setup_mqsim_oracle.py')
    previous=None
    if args.resume:
        previous=json.loads((out/'summary.json').read_text())
        expected=dict(source_manifest=manifest,upstream=build,qd=args.qd,
                      protect_top=args.protect_top,warmup_steps=args.warmup_steps,seed=args.seed,
                      cache_policy='shared_native_baseline_batch_lru',cache_update='native_top2048_only',
                      replacement_policy='miss_to_extra_miss_only')
        for key,value in expected.items():
            if previous.get(key)!=value:
                raise ValueError(f'Resume parameter mismatch: {key}; use original parameters')
        with tempfile.TemporaryDirectory(prefix='mqsim-resume-config-') as temp:
            check=Path(temp)/'ssd.xml'
            configure(source,check,manifest['snapshot_bytes'],args.ssd_config)
            if sha(check)!=previous.get('ssd_config_sha256') or sha(out/'ssd.xml')!=sha(check):
                raise ValueError('Resume SSD config mismatch; use original configuration')
        if sha(out/'prepare/ppa.csv')!=previous.get('mapping_sha256'):
            raise ValueError('Resume prepare mapping missing or changed')
    else:
        out.mkdir(parents=True,exist_ok=False)
    report=dict(status='running',upstream=build,source_manifest=manifest,
                qd=args.qd,budgets=sorted(set(args.budgets)),protect_top=args.protect_top,
                capacities=capacities,cache_policy='shared_native_baseline_batch_lru',
                cache_initialization='cold',cache_update='native_top2048_only',
                replacement_policy='miss_to_extra_miss_only',
                warmup_steps=args.warmup_steps,seed=args.seed,
                limitations=['Reference SSD geometry unless explicit config; not hardware-calibrated',
                             'Exact simulated PPA only; no PPA prediction error',
                             'Single SSD, fixed full snapshot; cold per-layer baseline HBM LRU',
                             'Counterfactual shared baseline cache; I/O to maintain unselected baseline rows is excluded',
                             'NVMe-to-NAND simulated latency; no Python selection/LRU/mapping-query time or model compute included',
                             '4 KiB requests remain separate, including same-page requests; no host coalescing',
                             'NAND transaction bytes are payload bytes; page sense bytes are an estimate',
                             'Random control matches actual replacement count, not score loss',
                             'Static sum-of-squares resource proxy, greedy shortlist; not global optimal',
                             'No model quality or end-to-end TPOT evaluation'],results=[])
    config=out/'ssd.xml'
    if previous is None: configure(source,config,manifest['snapshot_bytes'],args.ssd_config)
    report['ssd_config_sha256']=sha(config)
    # Every case replays the same full writes, and queries PPA only after completion.
    prep=out/'prepare'
    if previous is None:
        save_report(out,report)
        prep.mkdir()
        with (prep/'input.trace').open('w') as f:
            write_snapshot(f,manifest['snapshot_bytes'],manifest['segment_bytes'])
        prep_batches,mapping_path,prep_reads=run_sim(source,config,prep,args.qd,manifest['snapshot_bytes'],manifest['segment_bytes'])
        if len(prep_batches)!=1 or any(read_nand(prep_reads)): raise ValueError('Unexpected prepare activity')
    else:
        print('Resume: reuse prepared PPA mapping',flush=True)
        mapping_path=prep/'ppa.csv'
        with (prep/'batches.csv').open() as f:
            prep_batches=[{k:int(v) for k,v in r.items()} for r in csv.DictReader(f)]
        if (len(prep_batches)!=1 or prep_batches[0]['batch']!=0
                or prep_batches[0]['requests']!=manifest['snapshot_bytes']//manifest['segment_bytes']):
            raise ValueError('Resume prepare completion mismatch')
    mapping=load_mapping(mapping_path)
    if len(mapping)!=manifest['snapshot_bytes']//PAGE: raise ValueError('Incomplete PPA mapping')
    mapsha=sha(mapping_path); report['mapping_sha256']=mapsha
    completed={}; saved_contracts={}
    if previous is not None:
        for result in previous['results']:
            key=(result['capacity_rows_per_layer'],result['strategy'],result['budget'])
            if key in completed: raise ValueError('Duplicate completed resume case')
            if key[0] not in capacities or (key[2] and key[2] not in args.budgets): continue
            case=out/f'c{key[0]}-{key[1]}-b{key[2]}'
            saved_contracts[key]=completed_contracts(case,result,manifest,args.warmup_steps,mapsha)
            completed[key]=result
        # Preserve the original summary before changing the requested scope.
        (out/f'summary.before-resume-{time.time_ns()}.json').write_text(json.dumps(previous,indent=2)+'\n')
        report['results']=list(completed.values())
    save_report(out,report)
    for capacity in capacities:
        jobs=[('baseline',0)]
        for budget in sorted(set(args.budgets)):
            if budget: jobs.extend([('ppa_aware',budget),('random_matched',budget)])
        contracts=None; base_result=None
        for strategy,budget in jobs:
            key=(capacity,strategy,budget)
            case=out/f'c{capacity}-{strategy}-b{budget}'
            if key in completed:
                result=completed[key]; current_contracts=saved_contracts[key]
                if contracts is None: contracts=current_contracts
                if contracts!=current_contracts:
                    raise ValueError('Resumed cases have different baseline cache states')
                if base_result is None: base_result=result
                if any(result[k]!=base_result[k] for k in ('selected','cache_hits','fresh_hits','requests','nand_read_payload_bytes')):
                    raise ValueError('Resumed cases have unequal cache hit/read volume')
                print(f'Skip completed c{capacity} {strategy} budget={budget}',flush=True)
                continue
            if case.exists():
                archived=case.with_name(case.name+f'.interrupted-{time.time_ns()}')
                case.rename(archived)
                print(f'Preserved unfinished case: {archived.name}',flush=True)
            case.mkdir()
            print(f'Planning c{capacity} {strategy} budget={budget}',flush=True)
            metadata=[]; current_contracts=[]
            with (case/'input.trace').open('w') as trace, (case/'selections.jsonl').open('w') as selections:
                write_snapshot(trace,manifest['snapshot_bytes'],manifest['segment_bytes'])
                for m,audit,offsets in selections_with_baseline(inputs,mapping,capacity,strategy,budget,
                                                              args.protect_top,args.seed,args.warmup_steps):
                    current_contracts.append(m['baseline_contract_sha256'])
                    if contracts is not None and (m['batch']>len(contracts) or current_contracts[-1]!=contracts[m['batch']-1]):
                        raise ValueError('Per-step baseline cache/native hits/miss set differs between cases')
                    # Zero-Miss layers stay in metadata but issue no simulated I/O.
                    # Strictly increasing batch IDs may have gaps; native patch supports this.
                    for offset in offsets: trace.write(f'0 {m["batch"]} {offset//512} 8 1\n')
                    metadata.append(m); selections.write(json.dumps(audit)+'\n')
                    if m['batch']%1000==0:
                        print(f'Planned {case.name}: {m["batch"]}/{manifest["records"]} layer/step records',flush=True)
            if contracts is None: contracts=current_contracts
            elif current_contracts!=contracts: raise ValueError('Case lost baseline records')
            batches,actual_map_path,nand=run_sim(source,config,case,args.qd,manifest['snapshot_bytes'],manifest['segment_bytes'])
            active={b['batch']:b for b in batches[1:]}
            expected={m['batch'] for m in metadata if m['requests']}
            if sha(actual_map_path)!=mapsha or set(active)!=expected or len(active)!=len(batches)-1:
                raise ValueError('Mapping or nonempty batch completion changed across cases')
            if sum(x['requests'] for x in batches)!=sum(x['requests'] for x in metadata)+prep_batches[0]['requests']:
                raise ValueError('Request completion mismatch')
            cursor=batches[0]['end_ns']
            for m in metadata:
                if m['requests']:
                    b=active[m['batch']]
                    if b['requests']!=m['requests'] or b['begin_ns']<=cursor:
                        raise ValueError('Layer request/barrier mismatch')
                    cursor=b['end_ns']
                else:
                    b=dict(begin_ns=cursor,end_ns=cursor,elapsed_ns=0)
                m.update(begin_ns=b['begin_ns'],end_ns=b['end_ns'],elapsed_ns=b['elapsed_ns'],
                         nand_read_transactions=0,nand_read_payload_bytes=0,residence_ns=0,execution_ns=0,transfer_ns=0)
            # Attribute actual NAND transactions only to nonempty layer intervals.
            active_meta=[m for m in metadata if m['requests']]
            batch_index=0
            for tr in read_nand(nand):
                while batch_index<len(active_meta) and tr['end_ns']>active_meta[batch_index]['end_ns']:
                    batch_index+=1
                if batch_index>=len(active_meta) or tr['issue_ns']<active_meta[batch_index]['begin_ns']:
                    raise ValueError('NAND transaction crosses a layer barrier')
                m=active_meta[batch_index]; expected_ppa=mapping[tr['lpa']]
                if any(tr[k]!=expected_ppa[k] for k in ('ppa','channel','chip','die','plane','block','page')):
                    raise ValueError('NAND read position differs from prepared PPA')
                m['nand_read_transactions']+=1; m['nand_read_payload_bytes']+=tr['bytes']
                m['residence_ns']+=tr['end_ns']-tr['issue_ns']
                m['execution_ns']+=tr['execution_ns']; m['transfer_ns']+=tr['transfer_ns']
            if any(m['nand_read_transactions']!=m['requests'] or m['nand_read_payload_bytes']!=m['requests']*SLOT for m in metadata):
                raise ValueError('NAND read traffic differs from cache-filtered no-coalescing model')
            with (case/'layers.csv').open('w') as f:
                writer=csv.DictWriter(f,fieldnames=list(metadata[0])); writer.writeheader(); writer.writerows(metadata)
            measured=[m for m in metadata if not m['warmup']]
            steps=Counter()
            for m in measured: steps[m['step']]+=m['elapsed_ns']/1000
            total=sum(m['elapsed_ns'] for m in measured)/1e9
            reads=sum(m['requests'] for m in measured)
            selected=sum(m['selected'] for m in measured)
            hits=sum(m['cache_hits'] for m in measured); fresh=sum(m['fresh_hits'] for m in measured)
            result=dict(capacity_rows_per_layer=capacity,strategy=strategy,budget=budget,
                        baseline_cache_trace_sha256=hashlib.sha256(''.join(current_contracts).encode()).hexdigest(),
                        measured_batches=len(measured),zero_miss_batches=sum(m['requests']==0 for m in measured),
                        selected=selected,cache_hits=hits,fresh_hits=fresh,misses=reads,
                        cache_hit_rate=hits/selected if selected else None,
                        available_hit_rate=(hits+fresh)/selected if selected else None,
                        requests=reads,submitted_read_bytes=reads*SLOT,
                        nand_read_transactions=sum(m['nand_read_transactions'] for m in measured),
                        nand_read_payload_bytes=sum(m['nand_read_payload_bytes'] for m in measured),
                        nand_page_sense_bytes_estimate=reads*PAGE,read_seconds=total,
                        mean_nand_residence_us=sum(m['residence_ns'] for m in measured)/reads/1000 if reads else None,
                        mean_nand_residual_us=sum(m['residence_ns']-m['execution_ns']-m['transfer_ns'] for m in measured)/reads/1000 if reads else None,
                        layer_p95_us=percentile([m['elapsed_ns']/1000 for m in measured],.95),
                        step_p50_us=percentile(list(steps.values()),.5),step_p95_us=percentile(list(steps.values()),.95),
                        replacements=sum(m['replacements'] for m in measured),score_loss=sum(m['score_loss'] for m in measured),
                        mean_decision_ms=statistics.mean(m['decision_ms'] for m in measured),
                        submitted_MiB_s=reads*SLOT/(1<<20)/total if total else None)
            if base_result is None: base_result=result
            if any(result[k]!=base_result[k] for k in ('selected','cache_hits','fresh_hits','requests','nand_read_payload_bytes')):
                raise ValueError('Unequal cache hit/read volume; cannot claim a conflict-only comparison')
            result['speedup']=base_result['read_seconds']/total if total else None
            result['read_time_reduction']=1-total/base_result['read_seconds'] if base_result['read_seconds'] else None
            report['results'].append(result)
            save_report(out,report)
            speed=f'{result["speedup"]:.3f}' if result['speedup'] is not None else 'N/A (no reads)'
            print(f'Finished c{capacity} {strategy} budget={budget}: {total:.6f}s, speedup={speed}',flush=True)
    report['status']='complete'; save_report(out,report)
    return report

def parser():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--candidates',type=Path,required=True)
    p.add_argument('--mqsim',type=Path,default=ROOT/'.deps/MQSim')
    p.add_argument('--out-dir',type=Path,required=True)
    p.add_argument('--ssd-config',type=Path)
    p.add_argument('--resume',action='store_true',
                   help='Reuse validated completed cases in --out-dir; preserve and restart unfinished cases')
    p.add_argument('--qd',type=int,default=16,help='Outstanding NVMe requests; not CPU read threads')
    p.add_argument('--budgets',type=int,nargs='+',default=[0,32,64,128])
    p.add_argument('--protect-top',type=int,default=0)
    p.add_argument('--capacities',type=int,nargs='+',default=[4096,8192],
                   help='Native baseline LRU rows per layer; 0 is an explicit no-HBM control')
    p.add_argument('--warmup-steps',type=int,default=100,
                   help='Replay first N steps to warm the cold baseline LRU, excluding them from metrics')
    p.add_argument('--seed',type=int,default=2026)
    return p


if __name__=='__main__':
    main(parser().parse_args())
