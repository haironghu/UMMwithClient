#!/usr/bin/env python3
"""PPA-aware fixed-trajectory replay through patched official MQSim NVMe/FTL/NAND.

Single synthetic SSD; sequential full snapshot writes, then layer barriers.
No historical HBM cache, no NAND data cache, ideal mapping. Report simulator
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
import time
import xml.etree.ElementTree as ET

from setup_mqsim_oracle import PATCH, REVISION, ROOT

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


def select(native, candidate_rows, mapping, current, budget, protect_top=0):
    selected = set(native)
    scored = sorted((r for r in candidate_rows if r['score'] is not None),
                    key=lambda r: (-r['score'], r['token']))
    rank = {r['token']:i for i,r in enumerate(scored)}
    score = {r['token']:r['score'] for r in scored}
    info = {r['token']:mapping[r['offset_bytes']//PAGE] for r in candidate_rows}
    available = {r['token'] for r in scored} - selected - {current}
    removable = {t for t in native if t != current and t in rank and rank[t] >= protect_top}
    loads = pressure(selected-{current}, info)
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
        if len(set(native))!=min(2048,r['context_length']) or not set(native)<=ids or len(ids)!=len(candidates):
            raise ValueError('Invalid native or candidate set')
        scored=[x for x in candidates if x['score'] is not None]
        if len(scored)!=min(2400,r['context_length']) or any(not math.isfinite(x['score']) for x in scored):
            raise ValueError('Invalid reference scores')
        if r['current_token']!=r['context_length']-1 or r['context_length']>g['context_length'] or not set(native)-{r['current_token']}:
            raise ValueError('Invalid context geometry')
        for x in candidates:
            if not 0<=x['token']<r['context_length'] or x['device']!=0 or x['offset_bytes']!=g['base_bytes']+x['token']*SLOT:
                raise ValueError('Invalid token address')
    if count!=m['records'] or count==0: raise ValueError('Record count mismatch')
    return m


def main(args):
    source=args.mqsim.resolve();inputs=args.candidates.resolve();out=args.out_dir.resolve()
    if args.qd<1 or args.qd>65534 or any(b<0 or b>352 for b in args.budgets) or args.protect_top<0 or args.warmup_steps<0:
        raise ValueError('Invalid QD, budget, protection or warmup')
    if not any(r['step_id']>=args.warmup_steps for r in records(inputs)):
        raise ValueError('Warmup excludes all records')
    build=json.loads((source/'dsa-oracle-build.json').read_text())
    if (build['upstream_revision']!=REVISION or build['patch_sha256']!=sha(PATCH) or build['binary_sha256']!=sha(source/'MQSim')):
        raise ValueError('MQSim build mismatch; rerun setup_mqsim_oracle.py')
    manifest=validate_export(inputs)
    out.mkdir(parents=True,exist_ok=False)
    report=dict(status='running',upstream=build,source_manifest=manifest,
                qd=args.qd,budgets=sorted(set(args.budgets)),protect_top=args.protect_top,
                warmup_steps=args.warmup_steps,seed=args.seed,
                limitations=['Reference SSD geometry unless explicit config; not hardware-calibrated',
                             'Exact simulated PPA only; no PPA prediction error',
                             'Single SSD, fixed full snapshot, no historical HBM cache',
                             'NVMe-to-NAND simulated latency; no Python selection time or model compute included',
                             '4 KiB requests remain separate, including same-page requests; no host coalescing',
                             'NAND transaction bytes are payload bytes, not internal sensed bytes; page sense bytes are an estimate',
                             'Random control matches actual replacement count, not score loss',
                             'Static sum-of-squares resource proxy, greedy shortlist; not global optimal',
                             'No model quality or end-to-end TPOT evaluation'],results=[])
    report_path=out/'summary.json';report_path.write_text(json.dumps(report,indent=2))
    config=out/'ssd.xml'
    configure(source,config,manifest['snapshot_bytes'],args.ssd_config)
    report['ssd_config_sha256']=sha(config)
    # Dump real simulator mappings AFTER all writes complete. Each case replays
    # exactly these writes in a fresh process and checks mapping equality.
    prep=out/'prepare';prep.mkdir()
    with (prep/'input.trace').open('w') as f:
        write_snapshot(f,manifest['snapshot_bytes'],manifest['segment_bytes'])
    prep_batches,mapping_path,prep_reads=run_sim(source,config,prep,args.qd,manifest['snapshot_bytes'],manifest['segment_bytes'])
    if len(prep_batches)!=1 or any(read_nand(prep_reads)): raise ValueError('Unexpected prepare activity')
    mapping=load_mapping(mapping_path)
    if len(mapping)!=manifest['snapshot_bytes']//PAGE:
        raise ValueError('Incomplete PPA mapping')
    mapsha=sha(mapping_path);report['mapping_sha256']=mapsha
    jobs=[('baseline',0)]
    for budget in sorted(set(args.budgets)):
        if budget: jobs.extend([('ppa_aware',budget),('random_matched',budget)])
    for strategy,budget in jobs:
        case=out/f'{strategy}-b{budget}';case.mkdir()
        metadata=[]
        with (case/'input.trace').open('w') as trace, (case/'selections.jsonl').open('w') as selections:
            write_snapshot(trace,manifest['snapshot_bytes'],manifest['segment_bytes'])
            for index,row in enumerate(records(inputs),1):
                native=set(row['native']);current=row['current_token']
                start=time.perf_counter()
                selected,swaps,before,after=select(native,row['candidates'],mapping,current,budget,args.protect_top)
                if strategy=='baseline': selected=native;swaps=[]
                elif strategy=='random_matched':
                    ranked=sorted((x for x in row['candidates'] if x['score'] is not None),key=lambda x:(-x['score'],x['token']))
                    protected={x['token'] for x in ranked[:args.protect_top]}|{current}
                    scored={x['token'] for x in ranked}
                    eligible=sorted(native & scored - protected)
                    extra=sorted(scored-native-{current})
                    seed=int.from_bytes(hashlib.sha256(f'{args.seed}:{index}:{budget}'.encode()).digest()[:8],'little')
                    rng=random.Random(seed);n=len(swaps)
                    old=rng.sample(eligible,n);new=rng.sample(extra,n)
                    selected=(native-set(old))|set(new);swaps=list(zip(old,new))
                decision_ms=(time.perf_counter()-start)*1000
                if len(selected)!=len(native) or (current in selected)!=(current in native):
                    raise ValueError('Selection changes total count or current-token availability')
                info={x['token']:mapping[x['offset_bytes']//PAGE] for x in row['candidates']}
                addresses={x['token']:x['offset_bytes'] for x in row['candidates']}
                scores={x['token']:x['score'] for x in row['candidates']}
                loads=pressure(selected-{current},info)
                emitted=sorted(selected-{current}) # identical token-ID submission order rule
                score_loss=sum(scores[old]-scores[new] for old,new in swaps)
                for token in emitted: trace.write(f'0 {index} {addresses[token]//512} 8 1\n')
                m=dict(batch=index,step=row['step_id'],layer=row['layer_id'],requests=len(emitted),
                       warmup=row['step_id']<args.warmup_steps,replacements=len(swaps),score_loss=score_loss,
                       decision_ms=decision_ms,objective_before=before,objective_after=objective(loads),
                       unique_physical_pages=len({info[t]['ppa'] for t in emitted}),
                       peak_channel=max((v for k,v in loads.items() if len(k)==1),default=0),
                       peak_die=max((v for k,v in loads.items() if len(k)==3),default=0),
                       peak_plane=max((v for k,v in loads.items() if len(k)==4),default=0))
                metadata.append(m)
                selections.write(json.dumps(dict(**m,selected=sorted(selected),swaps=swaps))+'\n')
        batches,actual_map_path,nand=run_sim(source,config,case,args.qd,manifest['snapshot_bytes'],manifest['segment_bytes'])
        if sha(actual_map_path)!=mapsha or len(batches)!=len(metadata)+1:
            raise ValueError('Mapping or batch completion changed across cases')
        if sum(x['requests'] for x in batches)!=sum(x['requests'] for x in metadata)+prep_batches[0]['requests']:
            raise ValueError('Request completion mismatch')
        by_batch={m['batch']:m for m in metadata}
        for b in batches[1:]:
            m=by_batch[b['batch']]
            if b['requests']!=m['requests']:raise ValueError('Layer request mismatch')
            m.update(begin_ns=b['begin_ns'],end_ns=b['end_ns'],elapsed_ns=b['elapsed_ns'],nand_read_transactions=0,nand_read_payload_bytes=0,
                     residence_ns=0,execution_ns=0,transfer_ns=0)
        # Completion times belong to disjoint, closed layer intervals.
        batch_index=0
        for tr in read_nand(nand):
            while batch_index<len(metadata) and tr['end_ns']>metadata[batch_index]['end_ns']:
                batch_index+=1
            if batch_index>=len(metadata) or tr['issue_ns']<metadata[batch_index]['begin_ns']:
                raise ValueError('NAND transaction crosses a layer barrier')
            m=metadata[batch_index]
            expected=mapping[tr['lpa']]
            if any(tr[k]!=expected[k] for k in ('ppa','channel','chip','die','plane','block','page')):
                raise ValueError('NAND read position differs from prepared PPA')
            m['nand_read_transactions']+=1;m['nand_read_payload_bytes']+=tr['bytes']
            m['residence_ns']+=tr['end_ns']-tr['issue_ns']
            m['execution_ns']+=tr['execution_ns'];m['transfer_ns']+=tr['transfer_ns']
        if any(m['nand_read_transactions']!=m['requests'] or m['nand_read_payload_bytes']!=m['requests']*SLOT for m in metadata):
            raise ValueError('NAND read traffic differs from the no-cache/no-coalescing model')
        with (case/'layers.csv').open('w') as f:
            writer=csv.DictWriter(f,fieldnames=list(metadata[0]));writer.writeheader();writer.writerows(metadata)
        measured=[m for m in metadata if not m['warmup']]
        steps=Counter()
        for m in measured: steps[m['step']]+=m['elapsed_ns']/1000
        total=sum(m['elapsed_ns'] for m in measured)/1e9
        result=dict(strategy=strategy,budget=budget,measured_batches=len(measured),requests=sum(m['requests'] for m in measured),
                    submitted_read_bytes=sum(m['requests']*SLOT for m in measured),
                    nand_read_transactions=sum(m['nand_read_transactions'] for m in measured),
                    nand_read_payload_bytes=sum(m['nand_read_payload_bytes'] for m in measured),
                    nand_page_sense_bytes_estimate=sum(m['nand_read_transactions']*PAGE for m in measured),read_seconds=total,
                    mean_nand_residence_us=sum(m['residence_ns'] for m in measured)/sum(m['nand_read_transactions'] for m in measured)/1000,
                    mean_nand_residual_us=sum(m['residence_ns']-m['execution_ns']-m['transfer_ns'] for m in measured)/sum(m['nand_read_transactions'] for m in measured)/1000,
                    layer_p95_us=percentile([m['elapsed_ns']/1000 for m in measured],.95),
                    step_p50_us=percentile(list(steps.values()),.5),step_p95_us=percentile(list(steps.values()),.95),
                    replacements=sum(m['replacements'] for m in measured),score_loss=sum(m['score_loss'] for m in measured),
                    mean_decision_ms=statistics.mean(m['decision_ms'] for m in measured),
                    submitted_MiB_s=sum(m['requests']*SLOT for m in measured)/(1<<20)/total)
        base=report['results'][0] if report['results'] else result
        if result['requests']!=base['requests'] or result['nand_read_payload_bytes']!=base['nand_read_payload_bytes']:
            raise ValueError('Unequal read volume; cannot claim a conflict-only comparison')
        result['speedup']=base['read_seconds']/total
        report['results'].append(result)
        report_path.write_text(json.dumps(report,indent=2)+'\n')
        with (out/'comparison.csv').open('w') as f:
            writer=csv.DictWriter(f,fieldnames=list(result));writer.writeheader();writer.writerows(report['results'])
        print(f'Finished {strategy} budget={budget}: {total:.6f}s, speedup={result["speedup"]:.3f}',flush=True)
    report['status']='complete';report_path.write_text(json.dumps(report,indent=2)+'\n')
    return report


def parser():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--candidates',type=Path,required=True)
    p.add_argument('--mqsim',type=Path,default=ROOT/'.deps/MQSim')
    p.add_argument('--out-dir',type=Path,required=True)
    p.add_argument('--ssd-config',type=Path)
    p.add_argument('--qd',type=int,default=16,help='Outstanding NVMe requests; not CPU read threads')
    p.add_argument('--budgets',type=int,nargs='+',default=[0,32,64,128])
    p.add_argument('--protect-top',type=int,default=0)
    p.add_argument('--warmup-steps',type=int,default=0)
    p.add_argument('--seed',type=int,default=2026)
    return p


if __name__=='__main__':
    main(parser().parse_args())
