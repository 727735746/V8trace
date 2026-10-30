import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location('isolated_compile', Path(__file__).with_name('isolated_compile.py'))
isolated = importlib.util.module_from_spec(spec)
spec.loader.exec_module(isolated)


class IsolatedCompileTests(unittest.TestCase):
    def command(self):
        return isolated.COMPILER + ' /c ' + isolated.SOURCE + ' /Fo' + isolated.TARGET + ' /nologo /showIncludes:user /O2 -Xclang -add-plugin -Xclang raw-ptr-plugin'

    def test_extract_preserves_every_compiler_flag(self):
        command = self.command()
        log = 'FAILED: ' + isolated.TARGET + '\n' + command + '\nLLVM ERROR: out of memory\nAllocation failed\n'
        self.assertEqual(isolated.extract(log), command)

    def test_other_failure_is_not_retried(self):
        with self.assertRaisesRegex(ValueError, 'allocation failure'):
            isolated.extract('FAILED: ' + isolated.TARGET + '\n' + self.command() + '\nsyntax error\n')

    def test_other_target_is_not_retried(self):
        with self.assertRaisesRegex(ValueError, 'Document edge'):
            isolated.extract('FAILED: obj/other.obj\n' + isolated.COMPILER
                + ' /c ../../base/other.cc /Foobj/other.obj /showIncludes:user\nLLVM ERROR: out of memory\nAllocation failed\n')

    def test_other_ready_cxx_edge_keeps_its_command(self):
        command = isolated.COMPILER + ' /c ../../base/other.cc /Foobj/other.obj /showIncludes:user /O2'
        edge = isolated.extract_edge('FAILED: obj/other.obj\n' + command + '\nLLVM ERROR: out of memory\nAllocation failed\n')
        self.assertEqual(edge, dict(target='obj/other.obj', source='../../base/other.cc', command=command))

    def test_dollar_is_literal_without_changing_command(self):
        value = isolated.graph(self.command() + ' -DNAME=$VALUE').decode()
        self.assertIn('-DNAME=$$VALUE', value)
        self.assertIn('  deps = msvc\n', value)
        self.assertNotIn('subninja', value)

    def test_injected_graph_lines_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'single compiler command'):
            isolated.graph(self.command() + '\nrule injected')


class RecoveryFlowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='v8trace-isolated-flow-')
        self.addCleanup(self.temp.cleanup)
        self.checkout = Path(self.temp.name)
        self.output = self.checkout / 'out/Community'
        self.output.mkdir(parents=True)
        (self.checkout / 'input.cc').write_text('source', encoding='utf-8')
        (self.output / '.ninja_log').write_text('# ninja log v7\n1\t1\t1\tobj/other.obj\tabcd\n', encoding='utf-8')
        (self.output / '.ninja_deps').write_bytes(b'original-dependency-journal')
        self.calls = []
        self.change_unrelated = False
        owner = self
        class Supervisor:
            def active_ninja(self, output):
                return []
            def fingerprint(self, checkout, inputs):
                return [dict(path=name, sha256=isolated.digest(checkout / name)) for name in sorted(set(inputs))]
            def atomic_json(self, file, value):
                file.write_text(json.dumps(value), encoding='utf-8')
            def run_build(self, checkout, output, evidence, command, inputs, required, build_defaults=None):
                owner.calls.append(dict(command=list(command), inputs=list(inputs), defaults=build_defaults))
                evidence.mkdir(parents=True, exist_ok=True)
                if len(owner.calls) == 1:
                    log = evidence / 'failed.log'
                    line = isolated.COMPILER + ' /c ' + isolated.SOURCE + ' /Fo' + isolated.TARGET + ' /showIncludes:user /O2'
                    log.write_text('FAILED: ' + isolated.TARGET + '\n' + line + '\nLLVM ERROR: out of memory\nAllocation failed\n', encoding='utf-8')
                    self.atomic_json(evidence / 'build-state.json', dict(status='failed', exitCode=1, log=str(log), sources=self.fingerprint(checkout, inputs)))
                    raise RuntimeError('Ninja exited with 1')
                if '-f' in command:
                    file = output / isolated.TARGET
                    file.parent.mkdir(parents=True, exist_ok=True)
                    file.write_bytes(b'unit-object')
                    if owner.change_unrelated:
                        (output / '.ninja_log').write_text('# ninja log v7\n', encoding='utf-8')
                    with (output / '.ninja_deps').open('ab') as stream:
                        stream.write(b'new-document-deps')
                    value = dict(binaries=[dict(path=isolated.TARGET, sha256=isolated.digest(file))])
                else:
                    value = dict(status='compiled-not-validated', releaseDefaults=build_defaults)
                self.atomic_json(evidence / 'build-state.json', value)
                return value
        self.supervisor = Supervisor()

    def test_failed_full_then_original_mini_then_full_has_provenance(self):
        result = isolated.run_build(self.supervisor, self.checkout, self.output, self.checkout / 'evidence',
            ['ninja.exe', '-C', str(self.output), 'chrome', '-j', '1'], ['input.cc'], ['chrome.exe'], build_defaults=dict(edition='community'))
        self.assertEqual(len(self.calls), 3)
        self.assertIn('-f', self.calls[1]['command'])
        self.assertNotIn('-f', self.calls[2]['command'])
        self.assertTrue(result['releaseDefaults']['isolatedTranslationUnits'][0]['compilerFlagsUnchanged'])
        self.assertEqual(result['releaseDefaults']['edition'], 'community')
        self.assertEqual(len(self.calls[2]['inputs']), 2)

    def test_unrelated_journal_change_stops_full_resume(self):
        self.change_unrelated = True
        with self.assertRaisesRegex(RuntimeError, 'Unrelated Ninja command records'):
            isolated.run_build(self.supervisor, self.checkout, self.output, self.checkout / 'evidence',
                ['ninja.exe', '-C', str(self.output), 'chrome'], ['input.cc'], ['chrome.exe'])
        self.assertEqual(len(self.calls), 2)

    def test_parallelism_drops_to_one_after_allocation_failure(self):
        result = isolated.run_build(self.supervisor, self.checkout, self.output, self.checkout / 'evidence',
            ['ninja.exe', '-C', str(self.output), 'chrome', '-j', '2'], ['input.cc'], ['chrome.exe'], build_defaults=dict(ninjaJobs=2))
        self.assertEqual(self.calls[0]['command'][-1], '2')
        self.assertEqual(self.calls[-1]['command'][-1], '1')
        self.assertTrue(result['releaseDefaults']['parallelismReducedAfterAllocationFailure'])
        self.assertEqual(result['releaseDefaults']['ninjaJobs'], 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
