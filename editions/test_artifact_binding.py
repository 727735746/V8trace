import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('artifact_binding', Path(__file__).with_name('artifact_binding.py'))
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)


class BindingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='v8trace-binding-tests-')
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        for name, data in {'runtime-cpp/CMakeLists.txt': 'cmake_minimum_required(VERSION 3.24)', 'runtime-cpp/src/main.cpp': 'fixture-source',
                           'runtime-cpp/cmake/edition_build.h.in': 'fixture-edition-template', 'runtime-cpp/src/runtime.rc': 'fixture-resource',
                           'editions/community/LICENSE': 'fixture-license', '编译指令.txt': 'fixture-build-instructions',
                           'ui/TraceWindow.cs': 'fixture-ui', 'hooks.js': 'function installDiagnosticHooks() { return 1; }\nfunction createHookSource() {}',
                           'scenario_presets.json': '{}', 'case_prompt_templates.json': '{}'}.items():
            file = self.source / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(data, encoding='utf-8')
        self.identity = dict(runtime='cpp', version='fixture-version', platform='fixture', edition=dict(id='community', protocol='v8trace/edition/1', buildTimeIdentity=True))
        self.build = self.root / 'build'
        self.build.mkdir()
        self.compiler = self.root / 'compiler.exe'
        self.compiler.write_bytes(b'fixture-compiler')
        lines = ['CMAKE_CXX_COMPILER:FILEPATH=' + str(self.compiler)]
        for name in a.DEPENDENCIES:
            dependency = self.root / ('dependency-' + name)
            dependency.mkdir()
            (dependency / 'source.txt').write_text('pinned-read-only-use', encoding='utf-8')
            lines.append('FETCHCONTENT_SOURCE_DIR_' + name.upper() + ':PATH=' + str(dependency))
        (self.build / 'CMakeCache.txt').write_text('\n'.join(lines), encoding='utf-8')
        self.package = self.root / 'package'
        self.package.mkdir()
        for name, data in {'V8Trace.exe': b'fixture-ui-binary', 'runtime/v8trace-runtime.exe': b'fixture-runtime',
                           'app/v8-trace/scenario_presets.json': b'{}', 'app/v8-trace/case_prompt_templates.json': b'{}',
                           'app/v8-trace/diagnostic-hooks.js': a.generated_hooks(self.source)}.items():
            file = self.package / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(data)
        self.runtime = self.build / 'v8trace-runtime.exe'
        shutil.copyfile(self.package / 'runtime/v8trace-runtime.exe', self.runtime)
        self.snapshot_file = a.write_new(self.root / 'inputs.json', a.source_snapshot(self.source, 'community'))
        self.deps_file = a.write_new(self.root / 'dependencies.json', a.dependency_snapshot(self.build))
        self.junit = self.root / 'junit.xml'
        self.junit.write_text('<testsuite tests="1" failures="0" disabled="0"><testcase name="owned-fixture"/></testsuite>', encoding='utf-8')
        self.accepted = a.write_new(self.root / 'runtime-acceptance.json', a.runtime_receipt(self.snapshot_file, self.deps_file, self.runtime, self.junit, observe=lambda _: self.identity))
        source_inputs = a.read_json(self.snapshot_file)
        self.manifest = dict(edition='community', editionProtocol='v8trace/edition/1', runtime='cpp', nodeRequired=False, automaticRuntimeFallback=False,
                             artifactBindingProtocol='v8trace/product-binding/1', artifactBindingFileName='product.binding.json', runtimeVersion=self.identity['version'],
                             uiCompilerSha256=a.file_digest(self.compiler),
                             sourceInputsSha256=a.file_digest(self.snapshot_file), sourceDigest=source_inputs['sourceDigest'], uiInputDigest=source_inputs['uiInputDigest'],
                             runtimeInputDigest=source_inputs['runtimeInputDigest'], runtimeAcceptanceSha256=a.file_digest(self.accepted),
                             browserIncluded=False, packageBatchId='fixture-batch', uiVersion='fixture-ui-component', files=a.inventory(self.package))
        a.write_new(self.package / 'package-manifest.json', self.manifest)
        self.binding = a.write_new(self.root / 'product.binding.json', a.make_binding(self.package, self.snapshot_file, self.accepted, 'fixture-batch', self.compiler, observe=lambda _: self.identity))

    def tearDown(self):
        self.assertEqual(self.root.parent.resolve(), Path(tempfile.gettempdir()).resolve())
        self.assertTrue(self.root.name.startswith('v8trace-binding-tests-'))
        self.temp.cleanup()

    def test_consistent_fixture_versions_and_new_receipts(self):
        value = a.verify_binding(self.binding, source=True)
        self.assertEqual(value['packageBatchId'], 'fixture-batch')
        self.assertEqual(value['componentVersions']['runtime'], 'fixture-version')
        self.assertEqual(value['componentVersions']['ui'], 'fixture-ui-component')
        self.assertNotEqual(value['componentVersions']['runtime'], value['packageBatchId'])
        with self.assertRaises((ValueError, FileExistsError)):
            a.write_new(self.binding, value)

    def test_lone_utf16_surrogates_are_lossless_json_without_changing_normal_digests(self):
        value = dict(lone='中文' + chr(0xd800) + '\0TAIL', literal='\\ud800', pair='\U0001f642')
        file = a.write_new(self.root / 'utf16-observation.json', value)
        self.assertEqual(a.read_json(file), value)
        self.assertIn(b'\\ud800', file.read_bytes())
        self.assertNotEqual(a.object_digest(value['lone']), a.object_digest(value['literal']))
        plain = dict(text='正常中文')
        expected = json.dumps(plain, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8')
        self.assertEqual(a.canonical(plain), expected)

    def test_input_runtime_manifest_receipt_mutations(self):
        targets = [self.source / name for name in ('ui/TraceWindow.cs', 'runtime-cpp/cmake/edition_build.h.in', 'runtime-cpp/src/runtime.rc', 'editions/community/LICENSE', '编译指令.txt')]
        targets += [self.package / 'V8Trace.exe', self.package / 'runtime/v8trace-runtime.exe',
                   self.package / 'package-manifest.json', self.accepted, self.snapshot_file, self.deps_file, self.junit, self.runtime, self.compiler,
                   self.root / 'dependency-json/source.txt']
        for target in targets:
            with self.subTest(target=target):
                original = target.read_bytes()
                target.write_bytes(original + b'changed')
                with self.assertRaises((ValueError, json.JSONDecodeError, ETError)):
                    a.verify_binding(self.binding, source=True)
                target.write_bytes(original)
                a.verify_binding(self.binding, source=True)

    def test_unlisted_files_and_writable_hardlinks(self):
        extra = self.package / 'unlisted.txt'
        extra.write_text('extra', encoding='utf-8')
        with self.assertRaises(ValueError):
            a.verify_binding(self.binding)
        extra.unlink()
        alias = self.package / 'aliased-runtime.exe'
        os.link(self.package / 'runtime/v8trace-runtime.exe', alias)
        with self.assertRaises(ValueError):
            a.inventory(self.package, independent=True)
        alias.unlink()

    def test_edition_batch_and_disabled_tests(self):
        snapshot = a.read_json(self.snapshot_file)
        for field, value in [('edition', 'professional'), ('packageBatchId', 'another-batch')]:
            manifest = copy.deepcopy(self.manifest)
            manifest[field] = value
            (self.package / 'package-manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
            with self.assertRaises(ValueError):
                a.make_binding(self.package, self.snapshot_file, self.accepted, 'fixture-batch', self.compiler, observe=lambda _: self.identity)
        self.junit.write_text('<testsuite tests="1"><testcase><skipped/></testcase></testsuite>', encoding='utf-8')
        with self.assertRaises(ValueError):
            a.runtime_receipt(self.snapshot_file, self.deps_file, self.runtime, self.junit, observe=lambda _: self.identity)
        self.assertEqual(snapshot['edition'], 'community')

    def add_native(self):
        native = self.package / 'chromium'
        native.mkdir()
        for name in ('chrome.exe', 'chrome.dll'):
            (native / name).write_bytes(('owned-native-fixture-' + name).encode())
        descriptor = dict(edition='community', chromiumRevision='fixture-chromium', binaryHashes={name: a.file_digest(native / name) for name in ('chrome.exe', 'chrome.dll')})
        a.write_new(native / 'v8trace-opcode.json', descriptor)
        a.write_new(native / 'v8trace-runtime-files.json', dict(protocol='v8trace/runtime-files/1', descriptorSha256=a.file_digest(native / 'v8trace-opcode.json'), files=['chrome.exe', 'chrome.dll', 'v8trace-opcode.json']))
        manifest = copy.deepcopy(self.manifest)
        manifest['browserIncluded'] = True
        manifest['files'] = [row for row in a.inventory(self.package) if row['path'] != 'package-manifest.json']
        (self.package / 'package-manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
        return a.write_new(self.root / 'native.binding.json', a.make_binding(self.package, self.snapshot_file, self.accepted, 'fixture-batch', self.compiler, observe=lambda _: self.identity))

    def test_native_core_descriptor_inventory_and_mixed_edition(self):
        binding = self.add_native()
        a.verify_binding(binding)
        for name in ('chrome.exe', 'chrome.dll', 'v8trace-opcode.json', 'v8trace-runtime-files.json'):
            file = self.package / 'chromium' / name
            data = file.read_bytes()
            file.write_bytes(data + b'changed')
            with self.assertRaises(ValueError):
                a.verify_binding(binding)
            file.write_bytes(data)
        descriptor = a.read_json(self.package / 'chromium/v8trace-opcode.json')
        descriptor['edition'] = 'professional'
        (self.package / 'chromium/v8trace-opcode.json').write_text(json.dumps(descriptor), encoding='utf-8')
        with self.assertRaises(ValueError):
            a.native_components(self.package, 'community')

    def acceptance(self):
        product = a.verify_binding(self.binding)
        raw = a.write_new(self.root / 'actual-fixture-gate.json', dict(status='passed', edition='community', uiSha256=product['ui']['sha256'], runtimeSha256=product['runtime']['sha256'], fixtureOnly=True))
        envelope = dict(protocol='v8trace/product-acceptance/1', status='passed', scope='host', edition='community', packageBatchId=product['packageBatchId'],
                        productBindingSha256=a.file_digest(self.binding), uiSha256=product['ui']['sha256'], runtimeSha256=product['runtime']['sha256'], gates=[dict(status='passed', report=a.reference(raw))])
        report = a.write_new(self.root / 'acceptance-envelope.json', envelope)
        return a.write_new(self.root / 'acceptance.bound.json', a.bind_acceptance(self.binding, report)), raw

    def test_acceptance_hashes_and_actual_gate_identity(self):
        receipt, raw = self.acceptance()
        a.verify_acceptance(receipt, self.binding)
        data = raw.read_bytes()
        raw.write_bytes(data + b' ')
        with self.assertRaises(ValueError):
            a.verify_acceptance(receipt, self.binding)
        raw.write_bytes(data)
        envelope = a.read_json(self.root / 'acceptance-envelope.json')
        envelope['runtimeSha256'] = 'f' * 64
        bad = a.write_new(self.root / 'bad-envelope.json', envelope)
        with self.assertRaises(ValueError):
            a.bind_acceptance(self.binding, bad)

    def test_archive_contents_acceptance_and_tampering(self):
        accepted, _ = self.acceptance()
        archive = self.root / 'fixture.zip'
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zipped:
            for row in a.inventory(self.package):
                zipped.write(self.package / row['path'], row['path'])
        bound = a.write_new(self.root / 'archive.bound.json', a.bind_archive(self.binding, archive, accepted))
        self.assertFalse(a.verify_archive(bound)['wholePackageReleaseEligible'])
        with archive.open('ab') as stream:
            stream.write(b'trailing-change')
        with self.assertRaises(ValueError):
            a.verify_archive(bound)

    def test_archive_traversal_duplicate_and_symbolic_link(self):
        for index, name in enumerate(('../outside', 'runtime/../outside', 'C:/outside', '/outside')):
            file = self.root / ('bad-' + str(index) + '.zip')
            with zipfile.ZipFile(file, 'w') as zipped:
                zipped.writestr(name, 'fixture')
            with self.assertRaises(ValueError):
                a.bind_archive(self.binding, file)
        file = self.root / 'link.zip'
        with zipfile.ZipFile(file, 'w') as zipped:
            entry = zipfile.ZipInfo('V8Trace.exe')
            entry.create_system = 3
            entry.external_attr = (stat.S_IFLNK | 0o777) << 16
            zipped.writestr(entry, 'target')
        with self.assertRaises(ValueError):
            a.bind_archive(self.binding, file)

        file = self.root / 'directory-mode-file.zip'
        with zipfile.ZipFile(file, 'w') as zipped:
            for row in a.inventory(self.package):
                if row['path'] == 'V8Trace.exe':
                    entry = zipfile.ZipInfo(row['path'])
                    entry.create_system = 3
                    entry.external_attr = (stat.S_IFDIR | 0o755) << 16
                    zipped.writestr(entry, (self.package / row['path']).read_bytes())
                else:
                    zipped.write(self.package / row['path'], row['path'])
        with self.assertRaisesRegex(ValueError, 'Archive directory/file mode differs'):
            a.bind_archive(self.binding, file)


ETError = a.ET.ParseError

if __name__ == '__main__':
    unittest.main()
