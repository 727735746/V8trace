import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('browser_metadata', Path(__file__).with_name('browser_metadata.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class BrowserMetadataTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='v8trace-community-metadata-')
        self.root = Path(self.temporary.name)
        self.output = self.root / 'browser'
        self.output.mkdir()
        (self.output / 'args.gn').write_bytes(m.media_build.configure(b'fieldtrial_testing_config = false\n'))
        for name in ('chrome.exe', 'chrome.dll', 'chrome_elf.dll', 'v8_context_snapshot.bin', 'resources.pak'):
            (self.output / name).write_bytes(b'owned-fixture-v8trace/browser-edition/1-' + name.encode())
        patches = {}
        for role in ('chromium', 'v8'):
            file = self.root / ('community-' + role + '.patch')
            file.write_bytes(b'owned-public-patch-fixture')
            patches[role] = m.digest(file)
        self.projection = self.root / 'projection.json'
        rows = [dict(path='owned-source.cc', sha256='a' * 64)]
        m.write(self.projection, dict(edition='community', professionalProducersIncluded=False, files=rows,
                                    patchHashes=patches, chromiumRevision='fixture-chromium', v8Revision='fixture-v8'))
        self.receipt = self.root / 'receipt.json'
        self.receipt_data = dict(status='compiled-not-validated', exitCode=0, attemptId='owned-fixture-build', sources=rows,
                                binaries=[dict(path=name, sha256=m.digest(self.output / name)) for name in ('chrome.exe', 'chrome.dll', 'chrome_elf.dll', 'v8_context_snapshot.bin')],
                                releaseDefaults=dict(edition='community', sourceProjectionSha256=m.digest(self.projection), fieldTrialTestingConfig=False,
                                                     gnArgsSha256=m.digest(self.output / 'args.gn'), mediaBuild=m.media_build.verify(self.output / 'args.gn')))
        m.write(self.receipt, self.receipt_data)
        self.dependencies = self.root / 'dependencies.json'
        m.write(self.dependencies, {'//chrome:chrome': dict(runtime_deps=['chrome.exe', 'chrome.dll', 'chrome_elf.dll', 'v8_context_snapshot.bin', 'resources.pak'])})

    def tearDown(self):
        self.assertEqual(self.root.parent.resolve(), Path(tempfile.gettempdir()).resolve())
        self.assertTrue(self.root.name.startswith('v8trace-community-metadata-'))
        self.temporary.cleanup()

    def test_baseline_shared_memory_cap_does_not_claim_private_expansion(self):
        m.write_metadata(self.output, self.projection, self.receipt, self.dependencies)
        descriptor = m.load(self.output / 'v8trace-opcode.json')
        self.assertEqual(descriptor['traceSharedMemoryMaxBytes'], 8 << 20)
        self.assertEqual(descriptor['traceTransportProtocol'], 'v8trace/trace-transport/1')
        self.assertFalse(descriptor['nativeRunValidated'])
        self.assertFalse(descriptor['professionalProducersIncluded'])
        inventory = m.load(self.output / 'v8trace-runtime-files.json')
        self.assertEqual(inventory['descriptorSha256'], m.digest(self.output / 'v8trace-opcode.json'))

    def test_private_producer_remains_rejected(self):
        with (self.output / 'chrome.dll').open('ab') as stream:
            stream.write(b'V8TraceOpcode')
        for row in self.receipt_data['binaries']:
            row['sha256'] = m.digest(self.output / row['path'])
        m.write(self.receipt, self.receipt_data)
        with self.assertRaisesRegex(RuntimeError, 'Professional producer remains'):
            m.write_metadata(self.output, self.projection, self.receipt, self.dependencies)
        self.assertFalse((self.output / 'v8trace-opcode.json').exists())

    def test_http_drain_is_not_advertised_for_legacy_compiled_inputs(self):
        m.write_metadata(self.output, self.projection, self.receipt, self.dependencies)
        self.assertNotIn('httpDrainProtocol', m.load(self.output / 'v8trace-opcode.json'))
        self.assertNotIn('httpWireHeadersProtocol', m.load(self.output / 'v8trace-opcode.json'))

    def test_wire_headers_are_bound_to_compiled_header_and_raw_callback(self):
        native = m.native_source_dir()
        loader = self.root / 'apply-check/services/network/url_loader.cc'
        loader.parent.mkdir(parents=True)
        loader.write_text('v8trace_http::WireRequestHeaders(url_request_.get(), headers);\n', encoding='utf-8')
        rows = [dict(path='services/network/v8trace-http.h', sha256=m.digest(native / 'network-http.h')),
                dict(path='services/network/url_loader.cc', sha256=m.digest(loader))]
        projection = m.load(self.projection)
        projection['files'].extend(rows)
        m.write(self.projection, projection)
        self.receipt_data['sources'] = projection['files']
        self.receipt_data['releaseDefaults']['sourceProjectionSha256'] = m.digest(self.projection)
        m.write(self.receipt, self.receipt_data)
        m.write_metadata(self.output, self.projection, self.receipt, self.dependencies)
        self.assertEqual(m.load(self.output / 'v8trace-opcode.json')['httpWireHeadersProtocol'], 'v8trace/http-wire-headers/1')

    def test_wire_header_without_compiled_callback_is_not_advertised(self):
        native = m.native_source_dir()
        projection = m.load(self.projection)
        projection['files'].append(dict(path='services/network/v8trace-http.h', sha256=m.digest(native / 'network-http.h')))
        m.write(self.projection, projection)
        self.receipt_data['sources'] = projection['files']
        self.receipt_data['releaseDefaults']['sourceProjectionSha256'] = m.digest(self.projection)
        m.write(self.receipt, self.receipt_data)
        m.write_metadata(self.output, self.projection, self.receipt, self.dependencies)
        self.assertNotIn('httpWireHeadersProtocol', m.load(self.output / 'v8trace-opcode.json'))

    def test_compact_private_causal_producer_remains_rejected(self):
        with (self.output / 'chrome.dll').open('ab') as stream:
            stream.write(b'V8TraceCausalEvent')
        for row in self.receipt_data['binaries']:
            row['sha256'] = m.digest(self.output / row['path'])
        m.write(self.receipt, self.receipt_data)
        with self.assertRaisesRegex(RuntimeError, 'Professional producer remains'):
            m.write_metadata(self.output, self.projection, self.receipt, self.dependencies)

    def test_http_drain_is_bound_to_both_compiled_shared_headers(self):
        native = m.native_source_dir()
        rows = [dict(path='chrome/browser/' + name, sha256=m.digest(native / name))
                for name in ('v8trace-recording-control.h', 'v8trace-recording-browser.h')]
        projection = m.load(self.projection)
        projection['files'].extend(rows)
        m.write(self.projection, projection)
        self.receipt_data['sources'] = projection['files']
        self.receipt_data['releaseDefaults']['sourceProjectionSha256'] = m.digest(self.projection)
        m.write(self.receipt, self.receipt_data)
        m.write_metadata(self.output, self.projection, self.receipt, self.dependencies)
        self.assertEqual(m.load(self.output / 'v8trace-opcode.json')['httpDrainProtocol'], 'v8trace/http-drain/1')


if __name__ == '__main__':
    unittest.main()
