import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('workspace_plan', Path(__file__).with_name('workspace_plan.py'))
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)
a = w.a


class WorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='v8trace-workspace-tests-')
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        (self.source / 'runtime-cpp').mkdir(parents=True)
        (self.source / 'runtime-cpp/CMakeLists.txt').write_text('fixture-source', encoding='utf-8')
        compiler = self.root / 'compiler.exe'
        compiler.write_bytes(b'fixture-compiler')
        deps = {}
        for name in a.DEPENDENCIES:
            directory = self.root / ('dependency-' + name)
            directory.mkdir()
            (directory / 'input.txt').write_text('pinned-input', encoding='utf-8')
            files = a.inventory(directory)
            deps[name] = dict(root=str(directory), files=files, digest=a.object_digest(files))
        self.dep_file = a.write_new(self.root / 'dependencies.json', dict(protocol='v8trace/dependency-inputs/1', compiler=a.reference(compiler), dependencies=deps))
        self.request = dict(protocol='v8trace/workspace-request/1', jobs=1, copyMode='independent', dependencies=a.reference(self.dep_file),
                            protectedRoots=[str(self.source)], roots=[dict(path=str(self.root / edition), role='host-build', edition=edition, source=str(self.source), requiredBytes=2 * w.GIB) for edition in ('community', 'professional')])
        self.capacity = dict(volume='fixture-volume', freeBytes=100 * w.GIB, availableMemoryBytes=16 * w.GIB, availableCommitBytes=32 * w.GIB, fixtureOnly=True)

    def tearDown(self):
        self.assertEqual(self.root.parent.resolve(), Path(tempfile.gettempdir()).resolve())
        self.assertTrue(self.root.name.startswith('v8trace-workspace-tests-'))
        self.temp.cleanup()

    def plan(self, request=None, capacity=None, owners=()):
        return w.plan(request or self.request, probe=lambda _: capacity or self.capacity, owners=lambda: list(owners))

    def test_dry_plan_does_not_create_trees_or_reserve_memory(self):
        value = self.plan()
        self.assertTrue(value['readyToPrepare'])
        self.assertFalse(value['migratesActiveBuild'])
        self.assertFalse(value['resourceReservation'])
        self.assertFalse((self.root / 'community').exists())
        self.assertFalse((self.root / 'professional').exists())

    def test_aggregate_disk_memory_commit_and_live_owner(self):
        for key, amount, reason in [('freeBytes', 3 * w.GIB, 'insufficient-independent-workspace-capacity'), ('availableMemoryBytes', w.GIB, 'available-memory-below-request'), ('availableCommitBytes', w.GIB, 'available-commit-below-request')]:
            with self.subTest(key=key):
                capacity = dict(self.capacity, **{key: amount})
                self.assertIn(reason, self.plan(capacity=capacity)['blockers'])
        owner = dict(pid=123, roots=[str(self.root / 'community')], unknownWorkspace=False)
        self.assertFalse(self.plan(owners=[owner])['readyToPrepare'])
        self.assertFalse(self.plan(owners=[dict(pid=124, roots=[], unknownWorkspace=True)])['readyToPrepare'])

    def test_aliases_overlap_read_only_dependencies_and_hardlink_mode(self):
        for path in [self.request['roots'][0]['path'], str(self.root / 'community/child'), str(self.source / 'new'), str(self.root / 'dependency-json/child')]:
            request = copy.deepcopy(self.request)
            request['roots'][1]['path'] = path
            with self.assertRaises(ValueError):
                self.plan(request)
        request = dict(self.request, copyMode='hardlink')
        with self.assertRaises(ValueError):
            self.plan(request)

    def test_changed_pin_and_unowned_or_mixed_roots(self):
        file = self.root / 'dependency-json/input.txt'
        file.write_text('changed', encoding='utf-8')
        with self.assertRaises(ValueError):
            self.plan()
        file.write_text('pinned-input', encoding='utf-8')
        directory = self.root / 'community'
        directory.mkdir()
        with self.assertRaises(ValueError):
            self.plan()
        a.write_new(directory / w.MARKER, dict(protocol='v8trace/workspace-root/1', role='host-build', edition='professional', source=str(self.source)))
        with self.assertRaises(ValueError):
            self.plan()

    @unittest.skipUnless(os.name == 'nt', 'Windows junction path validation')
    def test_dangling_junction_ancestor_is_rejected(self):
        target, link = self.root / 'junction-target', self.root / 'junction'
        target.mkdir()
        subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(link), str(target)], capture_output=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            self.assertEqual(target.parent, self.root)
            self.assertTrue(self.root.name.startswith('v8trace-workspace-tests-'))
            target.rmdir()
            self.assertFalse(link.exists())
            self.assertTrue(link.lstat().st_file_attributes & 0x400)
            request = copy.deepcopy(self.request)
            request['roots'][1]['path'] = str(link / 'new-output')
            with self.assertRaisesRegex(ValueError, 'Reparse/symlink path rejected'):
                self.plan(request)
            self.assertFalse(target.exists())
        finally:
            self.assertEqual(link.parent, self.root)
            link.rmdir()

    def test_path_budget_and_active_gn_checkout_are_protected(self):
        request = copy.deepcopy(self.request)
        request['roots'][1]['path'] = str(self.root / ('long-' + 'x' * 160))
        self.assertIn('writable-root-path-budget-not-verified', self.plan(request)['blockers'])
        checkout = self.root / 'chromium'
        checkout.mkdir()
        (checkout / '.gn').write_text('owned-fixture', encoding='utf-8')
        request = copy.deepcopy(self.request)
        request['roots'][1]['path'] = str(checkout / 'another-output')
        owner = dict(pid=555, roots=[str(checkout / 'out/Release')], unknownWorkspace=False)
        self.assertIn('active-or-unattributed-build-owner:555', self.plan(request, owners=[owner])['blockers'])
        request['roots'][1]['path'] = str(self.root / 'logs/new-output')
        with self.assertRaises(ValueError):
            self.plan(request)

    def test_legacy_host_cache_source_edition_and_writable_links(self):
        directory = self.root / 'legacy'
        directory.mkdir()
        cache = directory / 'CMakeCache.txt'
        cache.write_text('V8TRACE_EDITION:STRING=community\nCMAKE_HOME_DIRECTORY:INTERNAL=' + str(self.source / 'runtime-cpp'), encoding='utf-8')
        value = w.init_host(directory, self.source, 'community', 1, probe=lambda _: self.capacity, owners=lambda: [])
        self.assertFalse(value['startsBuild'])
        with self.assertRaises(ValueError):
            w.init_host(directory, self.source, 'professional', 1, probe=lambda _: self.capacity, owners=lambda: [])
        original = directory / 'original.txt'
        original.write_text('shared writable data', encoding='utf-8')
        alias = directory / 'alias.txt'
        os.link(original, alias)
        with self.assertRaises(ValueError):
            w.init_host(directory, self.source, 'community', 1, probe=lambda _: self.capacity, owners=lambda: [])
        alias.unlink()

    @unittest.skipUnless(os.name == 'nt', 'Windows no-reparse preparation backend')
    def test_prepare_rechecks_capacity_and_owner_then_only_new_empty_roots(self):
        value = self.plan()
        plan_file = a.write_new(self.root / 'plan.json', value)
        with self.assertRaises(ValueError):
            w.prepare(plan_file, probe=lambda _: dict(self.capacity, freeBytes=1), owners=lambda: [])
        self.assertFalse((self.root / 'community').exists())
        with self.assertRaises(ValueError):
            w.prepare(plan_file, probe=lambda _: self.capacity, owners=lambda: [dict(pid=321, roots=[str(self.root / 'community')], unknownWorkspace=False)])
        self.assertFalse((self.root / 'community').exists())
        prepared = w.prepare(plan_file, probe=lambda _: self.capacity, owners=lambda: [])
        self.assertEqual(len(prepared['createdRoots']), 2)
        self.assertFalse(prepared['hardlinksCreated'])
        for edition in ('community', 'professional'):
            directory = self.root / edition
            self.assertEqual([item.name for item in directory.iterdir()], [w.MARKER])
            self.assertEqual((directory / w.MARKER).stat().st_nlink, 1)
            self.assertEqual(a.read_json(directory / w.MARKER)['edition'], edition)
        self.assertEqual(w.prepare(plan_file, probe=lambda _: self.capacity, owners=lambda: [])['createdRoots'], [])

    def test_changed_plan_and_unknown_fields_rejected(self):
        value = self.plan()
        value['roots'][0]['edition'] = 'professional'
        file = a.write_new(self.root / 'bad-plan.json', value)
        with self.assertRaises(ValueError):
            w.prepare(file, probe=lambda _: self.capacity, owners=lambda: [])
        with self.assertRaises(ValueError):
            self.plan(dict(self.request, pagefileAutoChange=True))


if __name__ == '__main__':
    unittest.main()
