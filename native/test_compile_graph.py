import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location('public_native_build', Path(__file__).with_name('build_community_chrome.py'))
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class CompileGraphTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='v8trace-graph-unit-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / 'out/Community'
        self.output.mkdir(parents=True)
        self.plan = b'[1/2] CXX shared.cc\n[2/2] LINK chrome.dll\n'
        self.link = b'command = lld-link.exe /nodefaultlib\n'
        self.calls = []
        original = builder.run
        self.addCleanup(setattr, builder, 'run', original)
        builder.run = self.fake_run

    def fake_run(self, command, cwd, evidence, name):
        self.calls.append(name)
        file = evidence / (name + '.log')
        if name == 'sync-manifest':
            (self.output / 'toolchain.ninja').write_bytes(self.link)
            file.write_bytes(b'Regenerated build files\n')
        else:
            self.assertIn(b'lld-link.exe /threads:1 ', (self.output / 'toolchain.ninja').read_bytes())
            file.write_bytes(self.plan)
        return file

    def test_manifest_regeneration_precedes_frozen_thread_limit(self):
        result = builder.prepare_compile_graph(self.root, self.output, self.root / 'proof')
        self.assertEqual(self.calls, ['sync-manifest', 'compile-plan'])
        self.assertEqual(result['generatedToolchainSha256'], builder.metadata.digest(self.output / 'toolchain.ninja'))
        self.assertEqual(result['linkerThreads'], 1)

    def test_a_second_regeneration_is_rejected_before_compile(self):
        self.plan = b'[0/1] Regenerating ninja files\n'
        with self.assertRaisesRegex(RuntimeError, 'regenerate frozen linker'):
            builder.prepare_compile_graph(self.root, self.output, self.root / 'proof')

    def test_unknown_linker_rules_are_not_silently_accepted(self):
        self.link = b'command = other-linker.exe\n'
        with self.assertRaisesRegex(RuntimeError, 'pinned linker'):
            builder.prepare_compile_graph(self.root, self.output, self.root / 'proof')
        self.assertEqual(self.calls, ['sync-manifest'])

    def test_library_driver_keeps_lib_first_without_thread_flag(self):
        source = b'command = lld-link.exe /lib /OUT:tiny.lib\n' + self.link
        result, modes = builder.limit_linker_threads(source)
        self.assertIn(b'lld-link.exe /lib /OUT:tiny.lib', result)
        self.assertNotIn(b'/threads:1 /lib', result)
        self.assertEqual(modes, dict(peLinkRules=1, rustLinkRules=0, librarianRules=1))

    def test_rust_flags_use_link_arg_not_bare_msvc_option(self):
        source = b'rspfile_content = -Clinker=../../lld-link.exe --crate-name trace ${rustflags}\n' + self.link
        result, modes = builder.limit_linker_threads(source)
        self.assertIn(b'-Clinker=../../lld-link.exe -Clink-arg=/threads:1 --crate-name', result)
        self.assertNotIn(b'lld-link.exe /threads:1 --crate-name', result)
        self.assertEqual(modes['rustLinkRules'], 1)

    def test_previous_bad_prefix_is_repaired_and_result_is_idempotent(self):
        source = (b'command = lld-link.exe /threads:1 /lib /OUT:tiny.lib\n'
            b'rspfile_content = -Clinker=../../lld-link.exe /threads:1 --crate-name trace\n' + self.link)
        first, modes = builder.limit_linker_threads(source)
        second, again = builder.limit_linker_threads(first)
        self.assertEqual(first, second)
        self.assertEqual(modes, again)

    def test_unreviewed_thread_value_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'thread argument'):
            builder.limit_linker_threads(b'command = lld-link.exe /threads:4 /OUT:x.exe\n')

    def test_unknown_rsp_mode_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'rule mode'):
            builder.limit_linker_threads(b'rspfile_content = lld-link.exe --crate-name trace\n' + self.link)


if __name__ == '__main__':
    unittest.main(verbosity=2)
