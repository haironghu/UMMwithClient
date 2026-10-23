import csv
import json
import os
from pathlib import Path
import tempfile
import unittest

from export_ppa_candidates import export, parser as export_parser
from make_ppa_oracle_fixture import make_fixture
from mqsim_ppa_oracle import configure, load_mapping, main, parser, run_sim, select, validate_export, write_snapshot
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
                '--out-dir',str(root/'results'),'--budgets','0','8','--qd','16']))
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
            with (root/'results/ppa_aware-b8/layers.csv').open() as f:rows=list(csv.DictReader(f))
            self.assertLess(int(rows[0]['end_ns']),int(rows[1]['begin_ns']))
            self.assertTrue(all(int(r['objective_after'])<int(r['objective_before']) for r in rows))


if __name__=='__main__':
    unittest.main()
