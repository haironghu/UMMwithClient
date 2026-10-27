import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from export_ppa_candidates import export, parser as export_parser
from make_ppa_oracle_fixture import make_fixture
from mqsim_ppa_oracle import baseline_states, configure, load_mapping, main, parser, run_sim, select, validate_export, write_snapshot
from simulate_hbm_candidates import load_record


class OracleTests(unittest.TestCase):
    def mapping(self):
        # Two fully separate resources, old tokens in channel 0, extras channel 1.
        return {i:dict(lpa=i,ppa=i,channel=int(i>=4),chip=0,die=0,plane=0,block=0,page=i) for i in range(8)}

    def test_selection_preserves_current_unknown_and_budget(self):
        mapping=self.mapping()
        candidates=[dict(token=i,score=None if i==0 else 20-i,offset_bytes=i*16384) for i in range(8)]
        native={0,1,2,3}
        chosen,swaps,before,after=select(native,candidates,mapping,current=3,budget=1)
        self.assertEqual(len(chosen),len(native))
        self.assertTrue({0,3}<=chosen) # unknown score and current token are protected
        self.assertEqual(len(swaps),1)
        self.assertTrue(set(chosen)-native <= {4,5,6,7})
        self.assertLess(after,before)
        unchanged,swaps,_,_=select(native,candidates,mapping,3,0)
        self.assertEqual(unchanged,native);self.assertEqual(swaps,[])

    def test_protect_top_and_balanced_noop(self):
        mapping=self.mapping()
        candidates=[dict(token=i,score=20-i,offset_bytes=i*16384) for i in range(8)]
        native={0,1,4,5}
        chosen,swaps,_,_=select(native,candidates,mapping,current=99,budget=8)
        self.assertEqual(chosen,native);self.assertFalse(swaps)
        native={0,1,2,3}
        chosen,swaps,_,_=select(native,candidates,mapping,current=3,budget=8,protect_top=4)
        self.assertEqual(chosen,native);self.assertFalse(swaps)

    def test_native_hits_and_extra_hits_are_never_swapped(self):
        mapping=self.mapping()
        candidates=[dict(token=i,score=20-i,offset_bytes=i*16384) for i in range(8)]
        native={0,1,2,3};cached={0,4}
        chosen,swaps,before,after=select(native,candidates,mapping,3,1,cached=cached)
        self.assertEqual(len(swaps),1)
        self.assertTrue({0,3}<=chosen)
        self.assertNotIn(4,chosen) # tempting extra hit must not reduce SSD read count
        self.assertEqual(len(chosen-cached-{3}),len(native-cached-{3}))
        self.assertEqual(before,16) # two SSD requests, same group at all four levels
        self.assertLess(after,before)
        unchanged,swaps,_,_=select(native,candidates,mapping,3,8,cached={4,5,6,7})
        self.assertEqual(unchanged,native);self.assertFalse(swaps)

    def test_lru_state_uses_native_only_including_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);make_fixture(root/'sample',steps=2,layers=(2,))
            export(export_parser().parse_args([str(root/'sample'),'--out-dir',str(root/'export'),'--segment-bytes','262144']))
            states=list(baseline_states(root/'export',4096))
            self.assertFalse(states[0][1])
            self.assertEqual(states[1][1],frozenset(states[0][0]['native']))
            # Every case reconstructs exactly the same pre-step state and digest.
            self.assertEqual(states,list(baseline_states(root/'export',4096)))

    def test_export_scores_addresses_numeric_order_and_full_snapshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);make_fixture(root/'sample')
            m=export(export_parser().parse_args([str(root/'sample'),'--out-dir',str(root/'export'),'--segment-bytes','262144']))
            rows=[json.loads(line) for line in (root/'export/candidates.jsonl').read_text().splitlines()]
            self.assertEqual([(r['step_id'],r['layer_id']) for r in rows],[(0,2),(0,10),(1,2),(1,10)])
            self.assertEqual(rows[1]['candidates'][0]['offset_bytes'],10001*4096)
            self.assertEqual(m['logical_kv_bytes'],2*10001*4096)
            self.assertEqual(m['snapshot_bytes']%262144,0)
            self.assertEqual(len(rows[0]['native']),2048)
            self.assertEqual(len(rows[0]['candidates']),2400)
            self.assertEqual(len(load_record(next((root/'sample').glob('*s10000.json')),10000)),3)
            self.assertEqual(validate_export(root/'export')['records'],4)
            with (root/'export/candidates.jsonl').open('a') as f:f.write('\n')
            with self.assertRaisesRegex(ValueError,'checksum'):
                validate_export(root/'export')

    def test_readonly_dense_mapping_rejects_gaps_aliases(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'map.csv'
            path.write_text('lpa,ppa,channel,chip,die,plane,block,page\n0,100,0,0,0,0,2,3\n1,101,1,0,0,0,2,3\n')
            mapping=load_mapping(path)
            self.assertEqual(mapping[1]['channel'],1)
            self.assertEqual(mapping[0]['ppa'],100)
            with self.assertRaises(KeyError):_ = mapping[2]
            path.write_text(path.read_text().replace('1,101','1,100'))
            with self.assertRaises(ValueError):load_mapping(path)

    @unittest.skipUnless(os.environ.get('MQSIM_SOURCE'),'requires patched MQSim build')
    def test_known_concentration_vs_spread_in_real_nand_scheduler(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=Path(os.environ['MQSIM_SOURCE'])
            snapshot=32*16384;segment=262144
            configure(source,root/'ssd.xml',snapshot)
            case=root/'case';case.mkdir()
            with (case/'input.trace').open('w') as f:
                write_snapshot(f,snapshot,segment)
                for batch,slots in ((1,[0,16,32,48]),(2,[0,4,8,12])):
                    for slot in slots:f.write(f'0 {batch} {slot*8} 8 1\n')
            batches,_,_=run_sim(source,root/'ssd.xml',case,16,snapshot,segment)
            self.assertEqual([r['requests'] for r in batches],[2,4,4])
            self.assertGreater(batches[1]['elapsed_ns'],batches[2]['elapsed_ns'])

    @unittest.skipUnless(os.environ.get('MQSIM_SOURCE'),'requires patched MQSim build')
    def test_real_nvme_ftl_nand_replay_equal_reads_and_barriers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);make_fixture(root/'sample',steps=1)
            export(export_parser().parse_args([str(root/'sample'),'--out-dir',str(root/'export'),'--segment-bytes','262144']))
            result=main(parser().parse_args(['--candidates',str(root/'export'),'--mqsim',os.environ['MQSIM_SOURCE'],
                '--out-dir',str(root/'results'),'--budgets','0','8','--qd','16','--capacities','0','--warmup-steps','0']))
            self.assertEqual(result['status'],'complete')
            base,aware,random=result['results']
            self.assertEqual(len({r['submitted_read_bytes'] for r in result['results']}),1)
            self.assertEqual(len({r['nand_read_transactions'] for r in result['results']}),1)
            self.assertEqual(base['requests'],2*2047)
            self.assertEqual(aware['replacements'],16)
            self.assertEqual(random['replacements'],16)
            for r in result['results']:
                self.assertGreater(r['read_seconds'],0)
                self.assertEqual(r['nand_read_payload_bytes'],r['submitted_read_bytes'])
            with (root/'results/c0-ppa_aware-b8/layers.csv').open() as f:rows=list(csv.DictReader(f))
            self.assertLess(int(rows[0]['end_ns']),int(rows[1]['begin_ns']))
            self.assertTrue(all(int(r['objective_after'])<int(r['objective_before']) for r in rows))

    @unittest.skipUnless(os.environ.get('MQSIM_SOURCE'),'requires patched MQSim build')
    def test_shared_cache_warmup_zero_miss_and_simulator_baseline_counts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);make_fixture(root/'sample',steps=5,layers=(2,10))
            base=[t for t in range(9000) if (t//4)%4==0][:2047]
            changed=base[:1919]+[t for t in range(9000) if (t//4)%4==1][:128]
            history=[base,changed,changed,[t for t in range(9000) if (t//4)%4==2][:2047],base]
            for path in (root/'sample').glob('*.json'):
                row=json.loads(path.read_text());seq=row['seq_lens_key'][0];step=seq-10000
                native=history[step]+[seq-1]
                pool=[t for t in range(5000,6000) if t not in native]+[t for t in range(seq-1) if t not in native]
                extra=list(dict.fromkeys(pool))[:352]
                row.update(indices=native,candidate_indices=native+extra,candidate_scores=list(range(2400,0,-1)))
                path.write_text(json.dumps(row))
            export(export_parser().parse_args([str(root/'sample'),'--out-dir',str(root/'export'),'--segment-bytes','262144']))
            result=main(parser().parse_args(['--candidates',str(root/'export'),'--mqsim',os.environ['MQSIM_SOURCE'],
                '--out-dir',str(root/'results'),'--budgets','8','--capacities','4096','8192','--warmup-steps','1']))
            subprocess.run([sys.executable,str(Path(__file__).with_name('simulate_hbm_candidates.py')),
                            str(root/'sample'),'--capacities','4096','8192','--warmup-steps','1',
                            '--out-dir',str(root/'sim')],check=True,capture_output=True)
            sim=json.loads((root/'sim/summary.json').read_text())['results']
            for capacity in (4096,8192):
                cases=[r for r in result['results'] if r['capacity_rows_per_layer']==capacity]
                baseline=next(r for r in sim if r['capacity_rows_per_layer']==capacity and r['strategy']=='baseline')
                for r in cases:
                    for key in ('selected','cache_hits','fresh_hits','misses','available_hit_rate'):
                        self.assertEqual(r[key],baseline[key])
                    self.assertGreater(r['zero_miss_batches'],0)
                self.assertGreater(cases[1]['replacements'],0)
                audits=[]
                for strategy,budget in [('baseline',0),('ppa_aware',8),('random_matched',8)]:
                    directory=root/f'results/c{capacity}-{strategy}-b{budget}'
                    rows=[json.loads(s) for s in (directory/'selections.jsonl').read_text().splitlines()]
                    audits.append(rows)
                    states=list(baseline_states(root/'export',capacity))
                    for row,(_,cached,_) in zip(rows,states):
                        for old,new in row['swaps']:
                            self.assertNotIn(old,cached);self.assertNotIn(new,cached)
                        self.assertEqual(set(row['miss_indices']),set(row['selected_indices'])-cached-{states[row['batch']-1][0]['current_token']})
                for rows in audits[1:]:
                    self.assertEqual([r['baseline_contract_sha256'] for r in rows],
                                     [r['baseline_contract_sha256'] for r in audits[0]])
                self.assertEqual([r['replacements'] for r in audits[1]], [r['replacements'] for r in audits[2]])
                with (root/f'results/c{capacity}-ppa_aware-b8/layers.csv').open() as f:
                    measured=list(csv.DictReader(f))
                self.assertTrue(any(int(r['requests'])==0 and int(r['elapsed_ns'])==0 for r in measured))

    @unittest.skipUnless(os.environ.get('MQSIM_SOURCE'),'requires patched MQSim build')
    def test_all_zero_miss_batches_no_nan_or_division_by_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);sample=root/'sample';sample.mkdir()
            record=dict(layer='model.layers.2.self_attn.attn',indices=[0],seq_lens_key=[1],
                        seq_lens_query=[1],has_reference_candidates=True,candidate_indices=[0],candidate_scores=[1.0])
            (sample/'model_layers_2_s1.json').write_text(json.dumps(record))
            export(export_parser().parse_args([str(sample),'--out-dir',str(root/'export'),'--segment-bytes','16384']))
            result=main(parser().parse_args(['--candidates',str(root/'export'),'--mqsim',os.environ['MQSIM_SOURCE'],
                '--out-dir',str(root/'results'),'--budgets','8','--capacities','4096','--warmup-steps','0']))
            for r in result['results']:
                self.assertEqual(r['requests'],0);self.assertEqual(r['read_seconds'],0)
                self.assertEqual(r['zero_miss_batches'],1);self.assertEqual(r['fresh_hits'],1)
                self.assertIsNone(r['speedup']);self.assertIsNone(r['submitted_MiB_s'])
            self.assertEqual(result['status'],'complete')


if __name__=='__main__':
    unittest.main()
