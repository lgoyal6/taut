#!/usr/bin/env python3
"""Reconstructs admission and completion counters from raw study timestamps.
Summarizes trial-level latency, goodput, wire/CPU cost and Jain fairness.
Keeps rejected offers, failed trials and conditional latency limitations visible.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import random
import re
import statistics


def percentile(xs, p):
    xs=sorted(xs)
    return xs[max(0, math.ceil(p*len(xs))-1)] if xs else 0.0


def jain(xs):
    return sum(xs)**2/(len(xs)*sum(x*x for x in xs)) if any(xs) else None


def summarize(xs):
    if not xs:
        return None
    rng=random.Random(20261015)
    means=sorted(statistics.mean(rng.choices(xs,k=len(xs))) for _ in range(10000))
    return dict(trials=len(xs),mean=statistics.mean(xs),median=statistics.median(xs),
                min=min(xs),max=max(xs),bootstrap_mean_95=[percentile(means,.025),percentile(means,.975)])


def read_csv(path):
    with path.open() as source:
        return list(csv.DictReader(source))


def analyze(root):
    protocol=json.loads((root/'protocol.json').read_text())
    rows=json.loads((root/'rows.json').read_text())
    cases={c['id']:c for c in protocol['cases']}
    failures=[];checks=[];groups={};fairness=[]
    expected_rows=sum(len(c['arms']) for c in cases.values())
    if len(rows)!=expected_rows:failures.append(f'row coverage {len(rows)}/{expected_rows}')
    for row in rows:
        case=cases[row['case_id']]
        base=root/case['id']/f"{row['flow']}-{row['transport']}.csv"
        if not base.exists():
            failures.append(f'missing CSV {base}');continue
        offers=read_csv(Path(str(base)+'.offers.csv'))
        samples=read_csv(Path(str(base)+'.samples.csv'))
        admitted={int(o['seq']):int(o['intended_ns']) for o in offers if int(o['accepted'])}
        seen=set();xs=[];in_window=0
        for sample in samples:
            seq=int(sample['seq']);ts=int(sample['intended_ns']);done=int(sample['completed_ns'])
            if seq in seen or admitted.get(seq)!=ts or done<ts:failures.append(f'invalid observation {base}:{seq}')
            seen.add(seq);xs.append((done-ts)/1e6)
        if offers:
            start=int(offers[0]['intended_ns'])
            # In paced mode first offer is t0; bulk starts slightly after t0.
            if case.get('mode','paced')!='bulk':
                end=start+case['duration_s']*1_000_000_000
                in_window=sum(int(s['completed_ns'])<end for s in samples)
                if in_window!=int(row['completed_in_window']):failures.append(f'window mismatch {base}')
                interval=int(1e9/case['rate'])
                for seq,o in enumerate(offers):
                    if int(o['seq'])!=seq or int(o['intended_ns'])!=start+seq*interval:failures.append(f'schedule mismatch {base}');break
                if len(offers)!=int(case['rate']*case['duration_s']):failures.append(f'offer denominator mismatch {base}')
        passed=(len(offers)==int(row['offered']) and len(admitted)==int(row['accepted']) and
                len(samples)==int(row['completed']) and len(offers)-len(admitted)==int(row['rejected']) and
                len(admitted)-len(samples)==int(row['missing']) and
                abs(percentile(xs,.99)-float(row['p99_ms']))<0.00001)
        if not passed:failures.append(f'counter/percentile reconstruction mismatch {base}')
        if str(row['errors'])!='0' or row['client_exit'] or row['server_exit']:failures.append(f'correctness/exit failure {base}')
        checks.append(dict(case=case['id'],transport=row['transport'],reconstructed=passed,
                           offered=len(offers),admitted=len(admitted),completed=len(samples),
                           offer_completion=len(samples)/len(offers) if offers else None,
                           admitted_completion=len(samples)/len(admitted) if admitted else None))
        if len(case['arms'])==2:
            continue # fairness uncertainty uses one Jain observation per two-flow trial
        key=(case.get('group',case['id']),row['transport'])
        group=groups.setdefault(key,dict(p99_ms=[],goodput_mbps=[],cpu_s=[],offer_completion=[],rejected=[]))
        group['p99_ms'].append(float(row['p99_ms']));group['goodput_mbps'].append(float(row['goodput_mbps']))
        group['cpu_s'].append(float(row['cpu_s']));group['rejected'].append(int(row['rejected']))
        group['offer_completion'].append(len(samples)/len(offers) if offers else 0)
    for case in cases.values():
        if len(case['arms'])!=2:continue
        rr=[r for r in rows if r['case_id']==case['id']]
        if len(rr)!=2:continue
        rates=[int(r.get('completed_in_window',0))*512*8/case['duration_s']/1e6 for r in rr]
        fairness.append(dict(case=case['id'],group=case.get('group',case['id']),run=case['run'],arms=case['arms'],
                             flow_goodput_mbps=rates,jain=jain(rates),aggregate_goodput_mbps=sum(rates),
                             note='512-byte request payload completed in common window; shared symmetric bottleneck, echoes consume reverse capacity'))
    wire=[]
    for case in cases.values():
        path=root/case['id']/'wire.json'
        if not path.exists():
            failures.append(f'missing wire counters {path}');continue
        data=json.loads(path.read_text())
        deltas={node:{direction:data['after'][node][0]['stats64'][direction]['bytes']-before[0]['stats64'][direction]['bytes'] for direction in ('tx','rx')} for node,before in data['before'].items()}
        rr=[r for r in rows if r['case_id']==case['id']]
        completed=sum(int(r.get('completed',0)) for r in rr)
        tx=sum(v['tx'] for v in deltas.values())
        server_cpu=[]
        for log in (root/case['id']).glob('*-server.log'):
            values=re.findall(r'cpu_s=([0-9.]+)',log.read_text())
            if values:server_cpu.append(float(values[-1]))
        wire.append(dict(case=case['id'],group=case.get('group',case['id']),run=case['run'],
                         endpoints=deltas,total_tx_bytes=tx,completed_request_payload_bytes=completed*512,
                         wire_per_completed_request_byte=tx/(completed*512) if completed else None,
                         client_cpu_s=sum(float(r.get('cpu_s',0)) for r in rr),server_cpu_s=sum(server_cpu),
                         scope='aggregate both directions, includes setup and drain; mixed flows are not individually attributable'))
    effects=[]
    for group in sorted({c.get('group',c['id']) for c in cases.values() if len(c['arms'])==1}):
        for arm in ('taut-fixed','taut-paced','quic'):
            differences=[]
            for run in (1,2,3):
                selected={r['transport']:r for r in rows if cases[r['case_id']].get('group',r['case_id'])==group and r['run']==run}
                if 'tcp' in selected and arm in selected:
                    differences.append(float(selected[arm]['p99_ms'])-float(selected['tcp']['p99_ms']))
            if differences:effects.append(dict(group=group,arm=arm,reference='tcp',p99_ms_difference=summarize(differences),scope='paired run-label difference, conditional on each arm completed work; admission may differ'))
    summary=[dict(group=k[0],transport=k[1],metrics={m:summarize(v) for m,v in g.items()}) for k,g in sorted(groups.items())]
    fair_groups={}
    for f in fairness:
        if f['jain'] is not None:fair_groups.setdefault(f['group'],[]).append(f['jain'])
    return dict(protocol_sha256=hashlib.sha256((root/'protocol.json').read_bytes()).hexdigest(),
                observed_rows=len(rows),expected_rows=expected_rows,reconstruction_checks=checks,failures=failures,
                summary=summary,paired_effects=effects,wire_and_cpu=wire,fairness=fairness,fairness_summary={k:summarize(v) for k,v in fair_groups.items()},
                limitations=['Latency is conditional on completed admitted work; rejected and missing offers remain separate outcomes.',
                             'Three repeated trials per condition give limited uncertainty estimates; bootstrap units are trials, not requests.',
                             'Netem random draws are not identically replayed across transports; run labels are blocking indices, not packet-loss seeds.',
                             'aioquic Python and native C++ have different runtime overhead; CPU is process CPU, wire counters include setup and teardown.',
                             'Bulk mode is a continuous bounded pipeline of echoed 512-byte requests, not a unidirectional byte-stream benchmark.',
                             'Same-host local network namespaces do not establish Internet fairness, production use or independent reproduction.'])


def main():
    p=argparse.ArgumentParser();p.add_argument('results',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    result=analyze(a.results);a.out.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(rows=result['observed_rows'],expected=result['expected_rows'],failures=result['failures']),indent=2))
    return int(bool(result['failures']))

if __name__=='__main__':raise SystemExit(main())
