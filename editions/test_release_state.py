import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import release_state as tools


class ReleaseStateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='v8trace-phase-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.checkout = self.root / 'src'
        self.output = self.checkout / 'out/fixture'
        self.output.mkdir(parents=True)
        self.source = self.checkout / 'input.cc'
        self.source.write_bytes(b'original input')
        self.command = ['ninja', '-C', str(self.output), 'chrome', '-j', '1']
        self.identity = dict(pid=80, creationToken='birth-1', commandLine='ninja fixture', executable='ninja')
        self.receipt = dict(protocol='v8trace/build-supervisor/1', checkout=str(self.checkout), output=str(self.output),
                            command=self.command, sources=self.fingerprint(self.checkout, ['input.cc']),
                            supervisorPid=80, ownerIdentity=self.identity)

    def fingerprint(self, checkout, names):
        return [dict(path=name, sha256=tools.file_sha256(checkout / name)) for name in sorted(names)]

    def resume(self, **changes):
        options = dict(receipt=self.receipt, checkout=self.checkout, output=self.output, command=self.command,
                       fingerprint=self.fingerprint, output_locked=True, query=lambda pid: None)
        options.update(changes)
        return tools.validate_resume(**options)

    def test_dead_owners_exact_inputs_and_exclusive_lease(self):
        report = self.resume()
        self.assertTrue(report['outputLeaseHeld'])
        self.assertFalse(report['startsProcess'])
        with self.assertRaisesRegex(RuntimeError, 'exclusive'):
            self.resume(output_locked=False)

    def test_live_matching_owner_is_not_duplicated(self):
        with self.assertRaisesRegex(RuntimeError, 'unchanged live owner'):
            self.resume(query=lambda pid: self.identity)

    def test_pid_reuse_creation_command_and_executable_are_all_checked(self):
        for field in ('creationToken', 'commandLine', 'executable'):
            with self.subTest(field=field):
                reused = dict(self.identity, **{field: 'different'})
                with self.assertRaisesRegex(RuntimeError, 'identity differs'):
                    self.resume(query=lambda pid: reused)
        self.receipt.pop('ownerIdentity')
        with self.assertRaisesRegex(RuntimeError, 'not recorded'):
            self.resume(query=lambda pid: self.identity)

    def test_changed_inputs_paths_and_command_stop_resume(self):
        self.source.write_bytes(b'changed')
        with self.assertRaisesRegex(RuntimeError, 'fingerprints'):
            self.resume()
        for changes in (dict(checkout=self.root), dict(output=self.root), dict(command=['ninja', '-j', '8'])):
            with self.assertRaises(ValueError):
                self.resume(**changes)

    def test_phase_history_is_immutable_and_byte_bound(self):
        evidence = self.root / 'phases'
        first = tools.phase_receipt(evidence, 'build', 'compiled-not-validated', {'input': self.source})
        original = first.read_bytes()
        second = tools.phase_receipt(evidence, 'observer', 'failed', parents=[first])
        record = json.loads(second.read_bytes())
        self.assertEqual(first.read_bytes(), original)
        self.assertEqual(record['parents'][0]['sha256'], tools.file_sha256(first))
        self.assertEqual(record['parents'][0]['status'], 'compiled-not-validated')
        self.assertFalse(record['remoteActionPerformed'])
        self.assertEqual(len(list(evidence.glob('*.json'))), 2)
        with self.assertRaises(ValueError):
            tools.phase_receipt(evidence, 'publication', 'published')

    def test_partial_receipt_never_appears_complete(self):
        evidence = self.root / 'phases'
        with mock.patch.object(tools.json, 'dump', side_effect=OSError('fixture write failure')):
            with self.assertRaises(OSError):
                tools.phase_receipt(evidence, 'validation', 'passed')
        self.assertEqual(list(evidence.glob('*.json')), [])
        self.assertEqual(len(list(evidence.glob('*.tmp'))), 1)

    def test_real_process_identity_and_dead_owner(self):
        original = tools.process_identity(os.getpid())
        self.assertEqual(original, tools.process_identity(os.getpid()))
        with subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],
                              creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0) as process:
            try:
                child = tools.process_identity(process.pid)
                self.assertEqual(child['pid'], process.pid)
                self.assertNotEqual(child['creationToken'], original['creationToken'])
            finally:
                process.terminate()
                process.wait(timeout=15)
        self.assertIsNone(tools.process_identity(process.pid))


if __name__ == '__main__':
    unittest.main()
