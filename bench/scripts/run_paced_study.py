#!/usr/bin/env python3
"""Runs frozen native and aioquic cases in local Docker network namespaces.
Verifies source/binary hashes and retains raw timing, resource and exit records.
Uses one shared bottleneck per direction and removes only its own resources.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid


def run_command(command, commands, check=True, timeout=60):
    try:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        output = error.output or ''
        if isinstance(output, bytes):
            output = output.decode(errors='replace')
        commands.write(json.dumps(dict(command=command, exit=124, output=output,
                                       timed_out=True, timeout_s=timeout, utc=time.time()))+'\n')
        raise
    commands.write(json.dumps(dict(command=command, exit=result.returncode,
                                   output=result.stdout, utc=time.time()))+'\n')
    if check and result.returncode:
        raise RuntimeError(f'command failed ({result.returncode}): {command}: {result.stdout}')
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--protocol', type=Path, required=True)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--image', default='b5-quic-tools:temporary')
    args = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    if output == root or root in output.parents:
        p.error('raw outputs must be outside the source checkout')
    output.mkdir(parents=True, exist_ok=False)
    protocol = json.loads(args.protocol.read_text())
    (output/'protocol.json').write_bytes(args.protocol.read_bytes())
    binary_hash = hashlib.sha256(args.binary.read_bytes()).hexdigest()
    if protocol.get('native_binary_sha256', binary_hash) != binary_hash:
        raise ValueError('frozen native binary hash mismatch')
    for source, expected in protocol.get('source_sha256', {}).items():
        actual = hashlib.sha256((root/source).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f'frozen source mismatch: {source}: {actual} != {expected}')
    commands = (output/'commands.jsonl').open('a', buffering=1)
    resources = []
    def run(command, check=True):
        return run_command(command, commands, check=check).stdout.strip()
    def docker(*parts, **kw):
        return run(['docker', *map(str, parts)], **kw)
    owner = uuid.uuid4().hex
    owner_label = 'taut.benchmark-owner'
    unique = 'b5-paced-'+owner[:10]
    network, client, server = unique+'-net', unique+'-c', unique+'-s'
    try:
        resources.append(('network',network))
        commands.write(json.dumps(dict(type='resource_intent', kind='network', name=network, owner=owner))+'\n')
        docker('network','create','--internal','--label',owner_label+'='+owner,network)
        for container in (client,server):
            resources.append(('container',container))
            commands.write(json.dumps(dict(type='resource_intent', kind='container', name=container, owner=owner))+'\n')
            docker('run','-d','--name',container,'--label',owner_label+'='+owner,'--network',network,'--cpus=1','--memory=512m','--cap-add=NET_ADMIN',args.image,'sleep','infinity')
            docker('cp',args.binary,container+':/opt/bench/paced_bench')
            docker('cp',root/'bench/quic_baseline.py',container+':/opt/bench/quic_baseline.py')
            docker('exec',container,'ethtool','-K','eth0','tso','off','gso','off','gro','off')
        ips = [docker('inspect','-f','{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}',x) for x in (client,server)]
        with tempfile.TemporaryDirectory(prefix='b5-paced-cert-') as temp:
            cert,key=Path(temp)/'cert.pem',Path(temp)/'key.pem'
            run(['openssl','req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:P-256','-nodes','-days','1','-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'])
            docker('cp',key,server+':/opt/bench/key.pem')
            for container in (client,server):
                docker('cp',cert,container+':/opt/bench/cert.pem')
        identity = dict(image=docker('image','inspect',args.image,'--format','{{.Id}}'), kernel=docker('exec',client,'uname','-a'), tcp_settings=docker('exec',client,'sysctl','net.ipv4.tcp_congestion_control','net.ipv4.tcp_rmem','net.ipv4.tcp_wmem'),
                        aioquic=docker('exec',client,'python3','-c','import aioquic;print(aioquic.__version__)'),
                        binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                        protocol_sha256=hashlib.sha256(args.protocol.read_bytes()).hexdigest(),
                        source_sha256={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in [root/'bench/paced_bench.cc',root/'bench/quic_baseline.py',Path(__file__).resolve()]},
                        cpu_caps='one CPU, 512 MiB per endpoint; competing clients share endpoint CPU',
                        topology='two Docker-managed network namespaces, internal bridge, shared egress netem bottleneck per endpoint, offloads disabled',
                        concurrent_load='exclusive portfolio measurement lock required; unrelated host processes are not controlled')
        (output/'identity.json').write_text(json.dumps(identity,indent=2))
        all_rows=[]
        for case_index, case in enumerate(protocol['cases']):
            case_dir=output/case['id'];case_dir.mkdir()
            loss=case['loss'];rtt=case['rtt_ms'];bandwidth=case['bandwidth']
            spec=['limit','100','delay',str(rtt/2)+'ms','rate',bandwidth]
            if loss=='burst': spec += ['loss','gemodel','5%','50%','90%','1%']
            elif loss: spec += ['loss',str(loss)+'%']
            if case.get('reorder'): spec += ['reorder','10%','50%','duplicate','2%']
            for node in (client,server): docker('exec',node,'tc','qdisc','replace','dev','eth0','root','netem',*spec)
            active=[]
            start_ns=int(docker('exec',client,'python3','-c','import time;print(time.monotonic_ns()+3_000_000_000)'))
            before={node:json.loads(docker('exec',node,'ip','-s','-j','link','show','eth0')) for node in (client,server)}
            for index,arm in enumerate(case['arms']):
                port=10000+case_index*20+index*10
                out='/tmp/'+case['id']+'-'+str(index)+'.csv'
                if arm=='quic':
                    s=['python3','/opt/bench/quic_baseline.py','server','--host',ips[1],'--port',str(port),'--cert','/opt/bench/cert.pem','--key','/opt/bench/key.pem','--idle-timeout','45']
                    c=['python3','/opt/bench/quic_baseline.py','client','--host',ips[1],'--port',str(port),'--ca','/opt/bench/cert.pem','--mode',case.get('mode','paced'),'--start-ns',str(start_ns),'--duration',str(case['duration_s']),'--drain','15','--rate',str(case['rate']),'--loss',str(0 if loss=='burst' else loss),'--rtt',str(rtt),'--run',str(case['run']),'--seed',str(case['run']),'--out',out]
                else:
                    shared=['/opt/bench/paced_bench','--mode',case.get('mode','paced'),'--transport','tcp' if arm=='tcp' else 'taut','--cc','paced' if arm=='taut-paced' else 'fixed','--port',str(port),'--rate',str(case['rate']),'--duration',str(case['duration_s']),'--drain','15','--start-ns',str(start_ns),'--out',out]
                    s=shared+['--role','receiver','--addr',ips[0],'--bind',ips[1]]
                    c=shared+['--role','sender','--addr',ips[1],'--bind',ips[0]]
                if arm == 'quic' and protocol.get('retransmission_accounting'):
                    s += ['--qlog', out+'.qlog']
                    c += ['--qlog', out+'.qlog']
                log=(case_dir/f'{index}-{arm}-server.log').open('w')
                sp=subprocess.Popen(['docker','exec',server,*s],stdout=log,stderr=subprocess.STDOUT)
                commands.write(json.dumps(dict(command=['docker','exec',server,*s],type='server_start'))+'\n')
                time.sleep(0.15)
                clog=(case_dir/f'{index}-{arm}-client.log').open('w')
                cp=subprocess.Popen(['docker','exec',client,*c],stdout=clog,stderr=subprocess.STDOUT)
                commands.write(json.dumps(dict(command=['docker','exec',client,*c],type='client_start'))+'\n')
                active.append((index,arm,out,sp,cp,log,clog))
            if case.get('rtt_change_ms'):
                target=start_ns+case['duration_s']*500_000_000
                while int(docker('exec',client,'python3','-c','import time;print(time.monotonic_ns())'))<target: time.sleep(0.1)
                changed=spec.copy();changed[3]=str(case['rtt_change_ms']/2)+'ms'
                for node in (client,server): docker('exec',node,'tc','qdisc','change','dev','eth0','root','netem',*changed)
            for index,arm,out,sp,cp,log,clog in active:
                try:
                    ce=cp.wait(timeout=45)
                except subprocess.TimeoutExpired:
                    cp.kill();cp.wait(timeout=10);ce=124
                try:
                    se=sp.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    sp.kill();sp.wait(timeout=10);se=124
                log.close();clog.close()
                commands.write(json.dumps(dict(case=case['id'],arm=arm,client_exit=ce,server_exit=se))+'\n')
                dest=case_dir/f'{index}-{arm}.csv'
                for suffix in ('offers.csv','samples.csv'):
                    run(['docker','cp',client+':'+out+'.'+suffix,str(dest)+'.'+suffix],check=False)
                run(['docker','cp',client+':'+out,str(dest)],check=False)
                if arm == 'quic' and protocol.get('retransmission_accounting'):
                    for node, direction in ((client, 'client'), (server, 'server')):
                        run(['docker','cp',node+':'+out+'.qlog',str(dest)+'.'+direction+'.qlog'],check=False)
                if dest.exists():
                    with dest.open() as result_csv:
                        row=list(csv.DictReader(result_csv))[-1]
                    row.update(case_id=case['id'],run=case['run'],flow=index,client_exit=ce,server_exit=se)
                    all_rows.append(row)
                else:
                    all_rows.append(dict(transport=arm,case_id=case['id'],run=case['run'],flow=index,client_exit=ce,server_exit=se,errors='no_csv'))
            after={node:json.loads(docker('exec',node,'ip','-s','-j','link','show','eth0')) for node in (client,server)}
            qdisc={node:docker('exec',node,'tc','-s','-j','qdisc','show','dev','eth0') for node in (client,server)}
            (case_dir/'wire.json').write_text(json.dumps(dict(before=before,after=after,qdisc=qdisc),indent=2))
            (output/'rows.json').write_text(json.dumps(all_rows,indent=2))
            print('CASE',case['id'],[(r['transport'],r.get('completed'),r.get('rejected'),r.get('errors')) for r in all_rows if r['case_id']==case['id']],flush=True)
            if any(str(row.get('errors')) != '0' or row['client_exit'] or row['server_exit'] for row in all_rows if row['case_id'] == case['id']):
                print('FAILURE_BOUNDARY: stopping before another case; remaining cases stay unperformed', flush=True)
                return 1
        return int(any(str(r.get('errors'))!='0' or r['client_exit'] or r['server_exit'] for r in all_rows))
    finally:
        cleanup_errors = []
        for kind,name in reversed(resources):
            try:
                labels = '.Config.Labels' if kind == 'container' else '.Labels'
                template = '{{.Id}} {{ index '+labels+' "'+owner_label+'" }}'
                inspected = run_command(['docker','inspect','--type',kind,'--format',template,name], commands, check=False)
                if inspected.returncode:
                    if any(f'No such {label}: {name}' in inspected.stdout for label in (kind, 'object')):
                        commands.write(json.dumps(dict(type='cleanup_skip', resource=name, reason='missing'))+'\n')
                        continue
                    raise RuntimeError(f'cannot inspect cleanup resource {name}: {inspected.stdout}')
                resource_id, _, actual_owner = inspected.stdout.strip().partition(' ')
                if actual_owner != owner:
                    commands.write(json.dumps(dict(type='cleanup_skip', resource=name, reason='ownership_mismatch'))+'\n')
                    continue
                # Delete the verified immutable ID, avoiding a name-replacement race.
                if kind=='container':docker('rm','-f',resource_id)
                else:docker('network','rm',resource_id)
            except Exception as error:
                cleanup_errors.append(str(error))
                commands.write(json.dumps(dict(type='cleanup_error', resource=name,
                                               error=str(error), utc=time.time()))+'\n')
                print(f'cleanup failed for {name}: {error}', file=sys.stderr)
        commands.close()
        if cleanup_errors and sys.exc_info()[0] is None:
            raise RuntimeError('resource cleanup failed: '+'; '.join(cleanup_errors))

if __name__=='__main__':
    raise SystemExit(main())
