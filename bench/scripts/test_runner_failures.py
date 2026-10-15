"""Offline controls for benchmark failure reporting and retained partial output."""
import contextlib
import csv
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import run_paced_study


class PacedCommandTests(unittest.TestCase):
    def test_timeout_is_bounded_and_logged_with_partial_output(self):
        log = io.StringIO()
        start = time.monotonic()
        with self.assertRaises(subprocess.TimeoutExpired):
            run_paced_study.run_command(
                [sys.executable, '-u', '-c',
                 'import time; print("started"); time.sleep(30)'],
                log, timeout=0.2)
        self.assertLess(time.monotonic() - start, 5)
        row = json.loads(log.getvalue())
        self.assertEqual(row['exit'], 124)
        self.assertTrue(row['timed_out'])
        self.assertEqual(row['timeout_s'], 0.2)
        self.assertIn('started', row['output'])

    def test_default_timeout_and_nonzero_are_recorded(self):
        log = io.StringIO()
        result = subprocess.CompletedProcess(['fixture'], 9, 'failed')
        with patch.object(run_paced_study.subprocess, 'run', return_value=result) as call:
            with self.assertRaises(RuntimeError):
                run_paced_study.run_command(['fixture'], log)
        self.assertEqual(call.call_args.kwargs['timeout'], 60)
        self.assertEqual(json.loads(log.getvalue())['exit'], 9)

    def test_cleanup_continues_and_preserves_original_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            protocol = root / 'protocol.json'
            protocol.write_text('{"cases": []}')
            binary = root / 'binary'
            binary.write_bytes(b'fixture')
            calls = []
            labels = {}

            def command(argv, log, **kwargs):
                calls.append(argv)
                if '--label' in argv:
                    name = argv[-1] if argv[1] == 'network' else argv[argv.index('--name') + 1]
                    labels[name] = argv[argv.index('--label') + 1].split('=', 1)[1]
                if argv[1] == 'inspect':
                    return subprocess.CompletedProcess(argv, 0, 'id-'+argv[-1]+' '+labels[argv[-1]])
                if argv[1] == 'cp':
                    raise ValueError('original setup failure')
                if argv[1] == 'rm':
                    raise subprocess.TimeoutExpired(argv, 60)
                return subprocess.CompletedProcess(argv, 0, '')

            args = ['runner', '--protocol', str(protocol), '--binary', str(binary),
                    '--output', str(root / 'out')]
            with patch.object(sys, 'argv', args), \
                    patch.object(run_paced_study, 'run_command', side_effect=command), \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaisesRegex(ValueError, 'original setup failure'):
                    run_paced_study.main()
            self.assertEqual(calls[-1][1:3], ['network', 'rm'])
            self.assertTrue(any(argv[1] == 'rm' for argv in calls))
            self.assertIn('cleanup_error', (root / 'out/commands.jsonl').read_text())


class MatrixProcessTests(unittest.TestCase):
    def run_matrix(self, failure, all_modes=False):
        with tempfile.TemporaryDirectory(prefix='b5-matrix-control-') as temp:
            root = Path(temp)
            bin_dir = root / 'bin'
            bin_dir.mkdir()
            docker = bin_dir / 'docker'
            docker.write_text('''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
with open(os.environ['CALLS'], 'a') as log:
    log.write(json.dumps(args) + '\\n')
if args[0] == 'exec' and 'cat' in args:
    print(100)
if args[0] == 'exec' and '--role' in args:
    role = args[args.index('--role') + 1]
    if '--out' in args and os.environ['FAILURE'] != 'missing_csv':
        import csv
        target = Path(os.environ['FIXTURE_DATA']) / Path(args[args.index('--out') + 1]).name
        fields = ['transport', 'cls', 'mode', 'loss_pct', 'rtt_ms', 'rate', 'run', 'seed']
        flags = ['--transport', '--class', '--mode', '--loss', '--rtt', '--rate', '--run', '--seed']
        row = [args[args.index(f) + 1] if f in args else '' for f in flags]
        fresh = not target.exists()
        with target.open('a') as output:
            writer = csv.writer(output)
            if fresh: writer.writerow(fields)
            writer.writerow(row)
        if row[0] == 'taut':
            resource = target.with_suffix('.resources.csv')
            fresh = not resource.exists()
            with resource.open('a') as output:
                writer = csv.writer(output)
                if fresh: writer.writerow(['role', 'errors'])
                writer.writerow(['rr-client', int(os.environ['FAILURE'] == 'resource_error')])
    if role == 'sender' and args[args.index('--transport') + 1] == 'taut' and args[args.index('--mode') + 1] != 'rr':
        import csv
        resource = Path(os.environ['FIXTURE_DATA']) / 'send.resources.csv'
        fresh = not resource.exists()
        with resource.open('a') as output:
            writer = csv.writer(output)
            if fresh: writer.writerow(['role', 'errors'])
            writer.writerow(['sender', 0])
    if role == os.environ['FAILURE']:
        sys.exit(9)
if args[0] == 'cp' and ':/bench-data/' in args[1]:
    import shutil
    origin = Path(os.environ['FIXTURE_DATA']) / args[1].rsplit('/', 1)[-1]
    if not origin.exists(): sys.exit(1)
    shutil.copyfile(origin, args[2])
''')
            docker.chmod(0o755)
            env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ['PATH'],
                       CALLS=str(root / 'calls'), FAILURE=failure, TOPOLOGY='docker',
                       FIXTURE_DATA=str(root),
                       DATA_DIR=str(root / 'output'), BIN=str(bin_dir), LOSSES='0',
                       RUNS='1', RUN_RR='1', RUN_OPENLOOP=str(int(all_modes)),
                       RUN_THROUGHPUT=str(int(all_modes)))
            result = subprocess.run(
                ['bash', str(Path(__file__).with_name('run_matrix.sh'))],
                env=env, capture_output=True, text=True, timeout=30)
            calls = [json.loads(line) for line in (root / 'calls').read_text().splitlines()]
            measurements = [a for a in calls if '--role' in a]
            copied = [i for i, a in enumerate(calls)
                      if a[0] == 'cp' and ':/bench-data/' in a[1]]
            removed = [i for i, a in enumerate(calls) if a[:2] == ['rm', '-f']]
            self.assertTrue(copied)
            self.assertLess(max(copied), min(removed))
            if failure != 'missing_csv':
                self.assertIn('transport,cls,mode', (root / 'output/latency.csv').read_text())
            self.assertEqual(len((root / 'output/wire.csv').read_text().splitlines()),
                             2 if failure in ('sender', 'receiver') else (7 if all_modes else 3))
            with (root / 'output/process.csv').open() as source:
                statuses = list(csv.DictReader(source))
            self.assertEqual(int(statuses[0]['sender_exit']), 9 if failure == 'sender' else 0)
            self.assertEqual(int(statuses[0]['receiver_exit']), 9 if failure == 'receiver' else 0)
            if failure == 'resource_error':
                with (root / 'output/latency.resources.csv').open() as source:
                    self.assertEqual(int(next(csv.DictReader(source))['errors']), 1)
            if failure == 'none':
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('run_matrix: done.', result.stdout)
                self.assertEqual(len(measurements), 12 if all_modes else 4)
            elif failure in ('sender', 'receiver'):
                self.assertEqual(result.returncode, 9, result.stderr)
                self.assertNotIn('run_matrix: done.', result.stdout)
                self.assertEqual(len(measurements), 2)
                self.assertIn('sender_exit=', result.stderr)
                self.assertIn('receiver_exit=', result.stderr)
            else:
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('run_matrix: done.', result.stdout)

    def test_sender_failure_stops_after_retaining_outputs(self):
        self.run_matrix('sender')

    def test_receiver_failure_stops_after_retaining_outputs(self):
        self.run_matrix('receiver')

    def test_valid_matrix_still_completes(self):
        self.run_matrix('none')

    def test_valid_matrix_covers_rr_openloop_and_throughput(self):
        self.run_matrix('none', all_modes=True)

    def test_zero_exit_without_csv_is_not_success(self):
        self.run_matrix('missing_csv')

    def test_zero_exit_with_resource_errors_is_not_success(self):
        self.run_matrix('resource_error')


if __name__ == '__main__':
    unittest.main()
