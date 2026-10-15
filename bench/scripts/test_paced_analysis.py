"""Checks the raw-observation oracle in analyze_paced_study.py.
Synthetic fixtures cover valid, duplicate and unadmitted completions.
Jain controls distinguish equal service, one losing flow and no service.
"""
import csv
import json
from pathlib import Path
import tempfile
import unittest
from analyze_paced_study import analyze, jain

class AnalysisControls(unittest.TestCase):
    def make_case(self,root):
        case=dict(id='control',group='control',arms=['tcp'],run=1,duration_s=1,rate=1)
        (root/'protocol.json').write_text(json.dumps(dict(cases=[case])))
        row=dict(transport='tcp',case_id='control',run=1,flow=0,client_exit=0,server_exit=0,
                 offered='1',accepted='1',rejected='0',completed='1',completed_in_window='1',
                 missing='0',errors='0',p99_ms='100',goodput_mbps='0.004096',cpu_s='0.01')
        (root/'rows.json').write_text(json.dumps([row]));d=root/'control';d.mkdir()
        (d/'0-tcp.csv').write_text('row data is checked through rows.json\n')
        (d/'0-tcp.csv.offers.csv').write_text('seq,intended_ns,observed_ns,accepted\n0,1000000000,1000000000,1\n')
        (d/'0-tcp.csv.samples.csv').write_text('seq,intended_ns,completed_ns\n0,1000000000,1100000000\n')
        (d/'wire.json').write_text(json.dumps(dict(before={'client':[dict(stats64=dict(tx=dict(bytes=0),rx=dict(bytes=0)))]},after={'client':[dict(stats64=dict(tx=dict(bytes=600),rx=dict(bytes=600)))]})))
        return d
    def test_valid_observation_reconstructs(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);self.make_case(root);result=analyze(root)
            self.assertEqual(result['failures'],[])
            self.assertEqual(result['reconstruction_checks'][0]['offer_completion'],1)
            self.assertEqual(result['wire_and_cpu'][0]['total_tx_bytes'],600)
    def test_duplicate_completion_is_detected(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);d=self.make_case(root)
            with (d/'0-tcp.csv.samples.csv').open('a') as f:f.write('0,1000000000,1100000000\n')
            self.assertTrue(any('invalid observation' in f for f in analyze(root)['failures']))
    def test_unadmitted_completion_is_detected(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);d=self.make_case(root)
            (d/'0-tcp.csv.offers.csv').write_text('seq,intended_ns,observed_ns,accepted\n0,1000000000,1000000000,0\n')
            failures=analyze(root)['failures']
            self.assertTrue(any('invalid observation' in f for f in failures))
            self.assertTrue(any('reconstruction mismatch' in f for f in failures))
    def test_jain_controls_include_no_service_and_one_losing_flow(self):
        self.assertIsNone(jain([0,0]))
        self.assertEqual(jain([1,1]),1)
        self.assertEqual(jain([1,0]),0.5)

if __name__=='__main__':unittest.main()
