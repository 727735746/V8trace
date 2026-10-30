import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location('media_build', Path(__file__).with_name('media_build.py'))
media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)


class MediaBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='v8trace-media-args-')
        self.addCleanup(self.temporary.cleanup)
        self.file = Path(self.temporary.name) / 'args.gn'

    def test_configure_preserves_existing_trace_flags_and_enables_codecs(self):
        self.file.write_bytes(media.configure(b'v8_enable_trace_ignition = true\r\nproprietary_codecs = false\r\n'))
        result = media.verify(self.file)
        self.assertTrue(result['proprietaryCodecs'])
        self.assertEqual(result['ffmpegBranding'], 'Chrome')
        self.assertIn(b'v8_enable_trace_ignition = true', self.file.read_bytes())
        self.assertFalse(result['runtimePlaybackValidated'])
        self.assertFalse(result['runtimeEncodingValidated'])

    def test_configuration_is_idempotent(self):
        value = media.configure(b'symbol_level = 0\n')
        self.assertEqual(media.configure(value), value)

    def test_duplicate_flags_are_not_silently_overridden(self):
        with self.assertRaisesRegex(ValueError, 'Duplicate media'):
            media.configure(b'proprietary_codecs = true\nproprietary_codecs = false\n')

    def test_missing_codec_build_flag_is_rejected(self):
        self.file.write_bytes(media.configure(b'').replace(b'media_use_openh264 = true\n', b''))
        with self.assertRaisesRegex(ValueError, 'media_use_openh264'):
            media.verify(self.file)

    def test_chromium_ffmpeg_variant_is_rejected(self):
        self.file.write_bytes(media.configure(b'').replace(b'"Chrome"', b'"Chromium"'))
        with self.assertRaisesRegex(ValueError, 'ffmpeg_branding'):
            media.verify(self.file)


if __name__ == '__main__':
    unittest.main(verbosity=2)
