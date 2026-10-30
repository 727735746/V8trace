import contextlib
import ctypes
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

import atomic_export as tools


class AtomicExportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='v8trace-export-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'input'
        self.source.mkdir()
        self.destination = self.root / 'public'

    def populate(self, output, data=b'\xef\xbb\xbf# UTF-8 \xe4\xb8\xad\xe6\x96\x87\n'):
        output.mkdir()
        file = output / 'fixture.ps1'
        file.write_bytes(data)
        manifest = dict(protocol='v8trace/community-host-source/1',
                        files=[dict(path=file.name, bytes=len(data), sha256=tools.digest(file))])
        (output / 'SOURCE-MANIFEST.json').write_text(json.dumps(manifest), encoding='utf-8')

    def test_commit_is_exact_and_existing_target_is_preserved(self):
        with tools.ExportTransaction(self.destination, self.source) as transaction:
            self.populate(transaction.output)
            expected = (transaction.output / 'fixture.ps1').read_bytes()
            self.assertFalse(self.destination.exists())
            transaction.commit()
        self.assertEqual((self.destination / 'fixture.ps1').read_bytes(), expected)
        self.assertEqual(len(tools.validate_manifest(self.destination)['files']), 1)
        with self.assertRaises(ValueError):
            tools.ExportTransaction(self.destination, self.source)
        self.assertEqual((self.destination / 'fixture.ps1').read_bytes(), expected)

    def test_copy_manifest_and_commit_failures_preserve_stages(self):
        for phase in ('copy', 'manifest', 'commit'):
            destination = self.root / phase
            with self.subTest(phase=phase), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises((OSError, ValueError)):
                    with tools.ExportTransaction(destination, self.source) as transaction:
                        self.populate(transaction.output)
                        if phase == 'copy':
                            raise OSError('fixture partial copy')
                        if phase == 'manifest':
                            (transaction.output / 'fixture.ps1').write_bytes(b'changed after manifest')
                            transaction.commit()
                        else:
                            with mock.patch.object(tools, 'rename_exclusive', side_effect=OSError('fixture commit failure')):
                                transaction.commit()
                self.assertFalse(destination.exists())
                stages = list(self.root.glob('.' + phase + '.export-*'))
                self.assertEqual(len(stages), 1)
                self.assertTrue((stages[0] / 'FAILURE.json').is_file())
                self.assertTrue((stages[0] / 'payload/fixture.ps1').is_file())

    def test_unlisted_secret_and_private_files_reject_commit(self):
        for name, data in (('unlisted.txt', b'unlisted'), ('fixture.ps1', b'ghp_' + b'A' * 24), ('private.exe', b'fixture')):
            with self.subTest(name=name), contextlib.redirect_stderr(io.StringIO()):
                destination = self.root / ('invalid-' + name)
                with self.assertRaises(ValueError):
                    with tools.ExportTransaction(destination, self.source) as transaction:
                        self.populate(transaction.output)
                        file = transaction.output / name
                        file.write_bytes(data)
                        if name != 'unlisted.txt':
                            manifest = json.loads((transaction.output / 'SOURCE-MANIFEST.json').read_bytes())
                            manifest['files'] = [dict(path=file.name, bytes=file.stat().st_size, sha256=tools.digest(file))]
                            (transaction.output / 'SOURCE-MANIFEST.json').write_text(json.dumps(manifest), encoding='utf-8')
                        transaction.commit()
                self.assertFalse(destination.exists())

    def test_racing_destination_is_never_replaced(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(OSError):
            with tools.ExportTransaction(self.destination, self.source) as transaction:
                self.populate(transaction.output)
                self.destination.mkdir()
                transaction.commit()
        self.assertTrue(self.destination.is_dir())
        self.assertEqual(list(self.destination.iterdir()), [])

    def test_source_tree_output_is_rejected(self):
        with self.assertRaises(ValueError):
            tools.ExportTransaction(self.source / 'nested', self.source)
        self.assertFalse((self.source / 'nested').exists())

    def test_bounded_sharing_retry(self):
        original = tools.rename_exclusive
        attempts = []
        def rename(source, target):
            attempts.append(1)
            if len(attempts) < 3:
                error = PermissionError('fixture sharing violation')
                error.winerror = 32
                raise error
            original(source, target)
        with tools.ExportTransaction(self.destination, self.source) as transaction:
            self.populate(transaction.output)
            with mock.patch.object(tools, 'rename_exclusive', side_effect=rename), mock.patch.object(tools.time, 'sleep'):
                transaction.commit()
        self.assertEqual(len(attempts), 3)

    @unittest.skipUnless(os.name == 'nt', 'Windows directory sharing test')
    def test_actual_windows_directory_sharing_conflict(self):
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CreateFileW.argtypes = (ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p)
        kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
        with tools.ExportTransaction(self.destination, self.source) as transaction:
            self.populate(transaction.output)
            handle = kernel.CreateFileW(str(transaction.output), 0x80000000, 3, None, 3, 0x02000000, None)
            self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
            released = threading.Event()
            def unlock():
                time.sleep(.25)
                kernel.CloseHandle(handle)
                released.set()
            thread = threading.Thread(target=unlock)
            thread.start()
            try:
                transaction.commit()
                self.assertTrue(released.is_set())
            finally:
                thread.join()


class ExporterIntegrationTests(unittest.TestCase):
    def test_real_host_export_and_exported_fixture_driver(self):
        with tempfile.TemporaryDirectory(prefix='v8trace-export-cli-') as root:
            destination = Path(root) / 'public'
            result = subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('export-community-host.py')), '--output', str(destination)],
                                    capture_output=True, check=True, timeout=90)
            self.assertEqual(json.loads(result.stdout)['output'], str(destination))
            tools.validate_manifest(destination)
            original = Path(__file__).resolve().parents[1] / 'runtime-cpp/tests/ui_studio_smoke.ps1'
            self.assertEqual((destination / 'runtime-cpp/tests/ui_studio_smoke.ps1').read_bytes(), original.read_bytes())
            child = subprocess.run([sys.executable, '-B', str(destination / 'editions/test_atomic_export.py'), 'AtomicExportTests'],
                                   capture_output=True, timeout=90)
            self.assertEqual(child.returncode, 0, child.stderr.decode('utf-8', 'replace'))
            child = subprocess.run([sys.executable, '-B', str(destination / 'editions/test_release_state.py')],
                                   capture_output=True, timeout=90)
            self.assertEqual(child.returncode, 0, child.stderr.decode('utf-8', 'replace'))

    def test_real_host_copy_failure_never_publishes(self):
        spec = importlib.util.spec_from_file_location('export_host_fixture', Path(__file__).with_name('export-community-host.py'))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory(prefix='v8trace-export-failure-') as root:
            destination = Path(root) / 'public'
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(OSError):
                with tools.ExportTransaction(destination, Path(__file__).resolve().parents[1]) as transaction:
                    with mock.patch.object(module.shutil, 'copyfile', side_effect=OSError('fixture copy failure')):
                        module.export_into(transaction.output)
                    transaction.commit()
            self.assertFalse(destination.exists())
            self.assertEqual(len(list(Path(root).glob('.public.export-*/FAILURE.json'))), 1)

    def test_full_export_invalid_native_receipt_retains_only_private_stage(self):
        script = Path(__file__).with_name('export-community.py')
        if not script.is_file():
            self.skipTest('Host-only source preview has no full native exporter')
        with tempfile.TemporaryDirectory(prefix='v8trace-native-export-failure-') as root:
            destination = Path(root) / 'public'
            receipt = Path(root) / 'invalid.json'
            receipt.write_text('{"conclusion":"FAIL"}', encoding='utf-8')
            result = subprocess.run([sys.executable, '-B', str(script), '--output', str(destination), '--native-validation', str(receipt)],
                                    capture_output=True, timeout=90)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(destination.exists())
            stages = list(Path(root).glob('.public.export-*'))
            self.assertEqual(len(stages), 1)
            self.assertTrue((stages[0] / 'payload/SOURCE-MANIFEST.json').is_file())
            self.assertTrue((stages[0] / 'FAILURE.json').is_file())


if __name__ == '__main__':
    unittest.main()
