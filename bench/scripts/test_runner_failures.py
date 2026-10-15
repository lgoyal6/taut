"""Offline controls for benchmark failure reporting and retained partial output."""
import contextlib
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

            def command(argv, log, **kwargs):
                calls.append(argv)
                if argv[1] == 'cp':
                    raise ValueError('original setup failure')
                if argv[1] == 'rm':
                    raise subprocess.TimeoutExpired(argv, 60)
                return ''

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
    def run_matrix(self, failure):
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
    if role == os.environ['FAILURE']:
        sys.exit(9)
if args[0] == 'cp' and ':/bench-data/' in args[1]:
    Path(args[2]).write_text('partial output\\n')
''')
            docker.chmod(0o755)
            env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ['PATH'],
                       CALLS=str(root / 'calls'), FAILURE=failure, TOPOLOGY='docker',
                       DATA_DIR=str(root / 'output'), BIN=str(bin_dir), LOSSES='0',
                       RUNS='1', RUN_RR='1', RUN_OPENLOOP='0', RUN_THROUGHPUT='0')
            result = subprocess.run(
                ['bash', str(Path(__file__).with_name('run_matrix.sh'))],
                env=env, capture_output=True, text=True, timeout=15)
            calls = [json.loads(line) for line in (root / 'calls').read_text().splitlines()]
            measurements = [a for a in calls if '--role' in a]
            copied = [i for i, a in enumerate(calls)
                      if a[0] == 'cp' and ':/bench-data/' in a[1]]
            removed = [i for i, a in enumerate(calls) if a[:2] == ['rm', '-f']]
            self.assertTrue(copied)
            self.assertLess(max(copied), min(removed))
            self.assertEqual((root / 'output/latency.csv').read_text(), 'partial output\n')
            self.assertEqual(len((root / 'output/wire.csv').read_text().splitlines()),
                             3 if failure == 'none' else 2)
            if failure == 'none':
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('run_matrix: done.', result.stdout)
                self.assertEqual(len(measurements), 4)
            else:
                self.assertEqual(result.returncode, 9, result.stderr)
                self.assertNotIn('run_matrix: done.', result.stdout)
                self.assertEqual(len(measurements), 2)
                self.assertIn('sender_exit=', result.stderr)
                self.assertIn('receiver_exit=', result.stderr)

    def test_sender_failure_stops_after_retaining_outputs(self):
        self.run_matrix('sender')

    def test_receiver_failure_stops_after_retaining_outputs(self):
        self.run_matrix('receiver')

    def test_valid_matrix_still_completes(self):
        self.run_matrix('none')


if __name__ == '__main__':
    unittest.main()
