import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

launcher = Path(__file__).resolve().parents[1] / 'cmake/msvc_utf8_launcher.py'
spec = importlib.util.spec_from_file_location('msvc_utf8_launcher', launcher)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class LauncherTests(unittest.TestCase):
    def test_localized_prefix_is_ascii(self):
        prefix = '注意: 包含文件:  '
        raw = (prefix + 'F:\\项目\\头文件.h\r\n').encode('cp936')
        self.assertEqual(module.normalize_line(raw, [prefix], 'cp936'),
                         'V8TRACE_INCLUDE: F:\\项目\\头文件.h\r\n'.encode('utf-8'))

    def test_utf8_and_english_prefix(self):
        self.assertEqual(module.normalize_line(b'Note: including file:   X.h\n', ['Note: including file: ']),
                         b'V8TRACE_INCLUDE:   X.h\n')
        self.assertEqual(module.normalize_line('错误 C1000: 文件\n'.encode('utf-8'), ['注意: 包含文件: ']),
                         '错误 C1000: 文件\n'.encode('utf-8'))

    def test_diagnostic_is_not_include(self):
        text = 'source.cpp(10): error: 注意: 包含文件:  X.h\r\n'
        self.assertEqual(module.normalize_line(text.encode('utf-8'), ['注意: 包含文件:  ']), text.encode('utf-8'))

    def test_exit_and_raw_diagnostic_preserved(self):
        with tempfile.TemporaryDirectory(prefix='v8trace-launcher-test-') as temporary:
            prefix = Path(temporary) / 'prefix.txt'
            prefix.write_text('Note: including file: \n', 'utf-8')
            child = "import sys;sys.stdout.buffer.write(b'Note: including file: X.h\\n');sys.stderr.buffer.write(b'error C1234\\n');sys.exit(7)"
            result = subprocess.run([sys.executable, '-B', str(launcher), '--include-prefix-file', str(prefix),
                                     sys.executable, '-c', child], capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 7)
            self.assertIn(b'V8TRACE_INCLUDE: X.h\n', result.stdout)
            self.assertIn(b'error C1234\n', result.stdout)
            self.assertEqual(result.stderr, b'')


if __name__ == '__main__':
    unittest.main()
