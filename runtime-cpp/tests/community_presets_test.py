import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('community_presets', Path(__file__).with_name('community_presets.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class PrologueTests(unittest.TestCase):
    def test_http_journal_does_not_shadow_http_server_module(self):
        class ServerReached(Exception):
            pass
        with tempfile.TemporaryDirectory(prefix='v8trace-community-preset-prologue-') as temporary:
            root = Path(temporary)
            with mock.patch.object(m.subprocess, 'check_output', return_value=b'{"edition":{"id":"community"}}'), mock.patch.object(m.http.server, 'ThreadingHTTPServer', side_effect=ServerReached('server reached')):
                with self.assertRaisesRegex(ServerReached, 'server reached'):
                    m.verify(root / 'fixture-runtime', root / 'assets', root / 'fixture-browser', root / 'evidence')
            self.assertEqual(root.parent.resolve(), Path(tempfile.gettempdir()).resolve())
            self.assertTrue(root.name.startswith('v8trace-community-preset-prologue-'))


if __name__ == '__main__':
    unittest.main()
