import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('stage_native_runtime', Path(__file__).with_name('stage-native-runtime.py'))
s = importlib.util.module_from_spec(spec)
spec.loader.exec_module(s)
a = s.a


class StagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='v8trace-native-stage-tests-')
        self.root = Path(self.temporary.name)
        self.source = self.root / 'gn-output'
        self.source.mkdir()
        for name in ('chrome.exe', 'chrome.dll', 'chrome_elf.dll', 'v8_context_snapshot.bin'):
            (self.source / name).write_bytes(('owned-fixture-' + name).encode())
        (self.source / 'unlisted-build-object.obj').write_bytes(b'not a runtime input')
        descriptor = dict(edition='community', binaryHashes={file.name: a.file_digest(file) for file in self.source.iterdir() if file.suffix != '.obj'})
        a.write_new(self.source / 'v8trace-opcode.json', descriptor)
        a.write_new(self.source / 'v8trace-runtime-files.json', dict(schemaVersion=1, protocol='v8trace/runtime-files/1', descriptorSha256=a.file_digest(self.source / 'v8trace-opcode.json'),
                    files=sorted([*descriptor['binaryHashes'], 'v8trace-opcode.json'])))

    def tearDown(self):
        self.assertEqual(self.root.parent.resolve(), Path(tempfile.gettempdir()).resolve())
        self.assertTrue(self.root.name.startswith('v8trace-native-stage-tests-'))
        self.temporary.cleanup()

    def stage(self, output=None, **kwargs):
        return s.stage(self.source, output or self.root / 'runtime', 'community', preflight=kwargs.pop('preflight', lambda *args: None), owners=kwargs.pop('owners', lambda: []), **kwargs)

    def test_exact_selected_runtime_ignores_gn_objects_without_hardlinks(self):
        before = a.inventory(self.source)
        value = self.stage()
        self.assertFalse(value['nativeBrowserValidated'])
        self.assertFalse((self.root / 'runtime/unlisted-build-object.obj').exists())
        self.assertEqual(a.inventory(self.source), before)
        for row in value['nativeInputs']['files']:
            self.assertEqual((self.root / 'runtime' / row['path']).stat().st_nlink, 1)
        with self.assertRaises(ValueError):
            self.stage()

    def test_changed_core_and_mixed_edition_rejected_before_output(self):
        with (self.source / 'chrome.dll').open('ab') as stream:
            stream.write(b'changed')
        with self.assertRaises(ValueError):
            self.stage()
        self.assertFalse((self.root / 'runtime').exists())
        with self.assertRaises(ValueError):
            s.stage(self.source, self.root / 'other', 'professional', preflight=lambda *args: None, owners=lambda: [])

    def test_capacity_or_active_source_owner_prevents_creation(self):
        def insufficient(*args):
            raise ValueError('capacity fixture')
        with self.assertRaises(ValueError):
            self.stage(preflight=insufficient)
        with self.assertRaises(ValueError):
            self.stage(owners=lambda: [dict(pid=10, roots=[str(self.source)], unknownWorkspace=False)])
        self.assertFalse((self.root / 'runtime').exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows legacy file path budget')
    def test_long_runtime_destination_rejected_before_copy(self):
        output = self.root / ('x' * 190)
        with self.assertRaisesRegex(ValueError, 'file path budget'):
            self.stage(output=output)
        self.assertFalse(output.exists())

    def test_traversal_and_case_alias_inventory_rejected(self):
        file = self.source / 'v8trace-runtime-files.json'
        original = json.loads(file.read_text())
        for name in ('../outside', 'CHROME.EXE'):
            value = dict(original, files=original['files'] + [name])
            file.write_text(json.dumps(value), encoding='utf-8')
            with self.assertRaises(ValueError):
                self.stage()
            self.assertFalse((self.root / 'runtime').exists())


if __name__ == '__main__':
    unittest.main()
