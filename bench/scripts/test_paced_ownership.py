"""Exercise Docker creation that succeeds before the CLI reports a timeout."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import run_paced_study


class PacedOwnershipTests(unittest.TestCase):
    def run_creation(self, target, outcome):
        with tempfile.TemporaryDirectory(prefix='b5-paced-ownership-') as temp:
            root = Path(temp)
            (root / 'protocol.json').write_text('{"cases": []}')
            (root / 'binary').write_bytes(b'fixture')
            resources, removed, calls = {}, [], []
            containers = 0

            def command(argv, **kwargs):
                nonlocal containers
                calls.append(argv)
                creation = argv[1:3] == ['network', 'create'] or argv[1] == 'run'
                if creation:
                    kind = 'network' if argv[1] == 'network' else 'container'
                    if kind == 'container':
                        containers += 1
                    point = 'network' if kind == 'network' else f'container{containers}'
                    name = argv[-1] if kind == 'network' else argv[argv.index('--name') + 1]
                    owner = argv[argv.index('--label') + 1].split('=', 1)[1] if '--label' in argv else None
                    if point != target or outcome != 'missing':
                        resources[name] = (kind, 'id-' + name,
                                           'foreign' if point == target and outcome == 'foreign' else owner)
                    if point == target:
                        raise subprocess.TimeoutExpired(argv, 60, output=b'created before timeout')
                elif argv[1] == 'inspect':
                    name = argv[-1]
                    if name not in resources:
                        kind = argv[argv.index('--type') + 1]
                        message = (f'network {name} not found' if kind == 'network'
                                   else f'No such container: {name}')
                        return subprocess.CompletedProcess(argv, 1, f'Error response from daemon: {message}\n')
                    kind, resource_id, owner = resources[name]
                    return subprocess.CompletedProcess(argv, 0, f'{resource_id} {owner}\n')
                elif argv[1] == 'rm' or argv[1:3] == ['network', 'rm']:
                    # Requiring the immutable ID also detects unsafe name-based removal.
                    name = next((n for n, v in resources.items() if v[1] == argv[-1]), None)
                    self.assertIsNotNone(name, argv)
                    self.assertNotEqual(resources[name][2], 'foreign')
                    removed.append(resources.pop(name))
                return subprocess.CompletedProcess(argv, 0, '')

            argv = ['runner', '--protocol', str(root / 'protocol.json'),
                    '--binary', str(root / 'binary'), '--output', str(root / 'out')]
            with patch.object(sys, 'argv', argv), \
                    patch.object(run_paced_study.subprocess, 'run', side_effect=command):
                with self.assertRaises(subprocess.TimeoutExpired):
                    run_paced_study.main()
            records = [json.loads(x) for x in (root / 'out/commands.jsonl').read_text().splitlines()]
            self.assertFalse(any(x.get('type') == 'cleanup_error' for x in records), records)
            self.assertTrue(any(x.get('timed_out') for x in records))
            intents = [x for x in records if x.get('type') == 'resource_intent']
            self.assertEqual(len(intents), 1 if target == 'network' else int(target[-1]) + 1)
            if outcome == 'foreign':
                self.assertEqual(len(resources), 1)
                self.assertEqual(next(iter(resources.values()))[2], 'foreign')
                self.assertTrue(any(x.get('reason') == 'ownership_mismatch' for x in records))
            else:
                self.assertEqual(resources, {})
            if outcome == 'created':
                self.assertEqual(len(removed), len(intents))
                self.assertTrue(all(x[2] for x in removed))
            if outcome == 'missing':
                self.assertTrue(any(x.get('reason') == 'missing' for x in records))

    def test_network_created_before_timeout_is_removed(self):
        self.run_creation('network', 'created')

    def test_first_container_created_before_timeout_is_removed(self):
        self.run_creation('container1', 'created')

    def test_second_container_created_before_timeout_is_removed(self):
        self.run_creation('container2', 'created')

    def test_missing_creation_is_a_cleanup_noop(self):
        for target in ['network', 'container1', 'container2']:
            with self.subTest(target=target):
                self.run_creation(target, 'missing')

    def test_foreign_collision_is_preserved(self):
        for target in ['network', 'container1', 'container2']:
            with self.subTest(target=target):
                self.run_creation(target, 'foreign')


if __name__ == '__main__':
    unittest.main()
