import http.client
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
from urllib.parse import urlsplit


spec = importlib.util.spec_from_file_location('media_browser', Path(__file__).with_name('media_browser.py'))
media_browser = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media_browser)


class MediaCaptureOptionsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='v8trace-media-unit-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.chrome = self.root / 'browser/chrome.exe'
        self.chrome.parent.mkdir()
        for name in ['chrome.exe', 'chrome.dll', 'chrome_elf.dll', 'v8_context_snapshot.bin', 'v8trace-opcode.json']:
            (self.chrome.parent / name).write_bytes(name.encode())
        self.runtime = self.root / 'runtime.exe'
        self.runtime.write_bytes(b'runtime')
        self.fixtures = self.root / 'fixtures'
        self.fixtures.mkdir()
        for name in ['fixture.mp4', 'fixture.webm']:
            (self.fixtures / name).write_bytes(name.encode())
        self.options = []
        self.critical_gaps = 0

    def capture(self, command, **unused):
        config = json.loads(Path(command[3]).read_text('utf-8'))
        self.options.append(config)
        self.assertIs(config.get('redirectBodies'), True)
        self.assertIs(config['disableExtensions'], True)
        self.assertIs(config['headless'], False)
        output = Path(config['output'])
        output.mkdir()
        summary = dict(integrityConclusion='partial' if self.critical_gaps else 'complete',
            criticalGapCount=self.critical_gaps, counts=dict(jscall=1, native=1), gaps=[])
        (output / 'summary.json').write_text(json.dumps(summary), encoding='utf-8')
        url = urlsplit(config['url'])
        connection = http.client.HTTPConnection(url.hostname, url.port, timeout=5)
        try:
            connection.request('POST', '/', body=b'{"ok":true}')
            self.assertEqual(connection.getresponse().status, 204)
        finally:
            connection.close()
        return subprocess.CompletedProcess(command, 0, b'', b'')

    def verify(self):
        args = SimpleNamespace(evidence=self.root / 'evidence', fixtures=self.fixtures,
            runtime=self.runtime, chrome=self.chrome, assets=self.root, edition='professional', headful=True)
        with mock.patch.object(media_browser.subprocess, 'check_output', return_value=b'{"edition":{"id":"professional"}}'), mock.patch.object(media_browser.subprocess, 'run', side_effect=self.capture):
            return media_browser.verify(args)

    def test_both_presets_request_real_redirect_body_capture(self):
        result = self.verify()
        self.assertEqual(result['conclusion'], 'PASS')
        self.assertEqual([row['preset'] for row in self.options], ['capture-baseline', 'env-target-detail'])
        self.assertEqual([row['mode'] for row in result['cases']], ['baseline', 'detailed'])
        self.assertTrue(result['temporaryFixturesRemoved'])

    def test_critical_capture_gaps_remain_failures(self):
        self.critical_gaps = 1
        with self.assertRaisesRegex(RuntimeError, 'Capture during video playback was not complete'):
            self.verify()
        result = json.loads((self.root / 'evidence/result.json').read_text('utf-8'))
        self.assertEqual(result['conclusion'], 'FAIL')
        self.assertTrue(result['temporaryFixturesRemoved'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
