import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import uuid

spec = importlib.util.spec_from_file_location('artifact_binding', Path(__file__).with_name('artifact_binding.py'))
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)
GIB = 1073741824
MARKER = '.v8trace-workspace.json'


def overlaps(left, right):
    return left == right or left.is_relative_to(right) or right.is_relative_to(left)


def resources(path):
    parent = a.plain_path(path, exists=False)
    while not parent.exists():
        parent = parent.parent
    usage = shutil.disk_usage(parent)
    if os.name == 'nt':
        class Memory(ctypes.Structure):
            _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong)] + [(key, ctypes.c_ulonglong) for key in ('totalPhysical', 'availablePhysical', 'totalCommit', 'availableCommit', 'totalVirtual', 'availableVirtual', 'extendedVirtual')]
        info = Memory()
        info.length = ctypes.sizeof(info)
        a.require(ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(info)), 'Memory capacity observation failed')
        memory, commit = info.availablePhysical, info.availableCommit
        volume = parent.anchor.casefold()
    else:
        values = {}
        for line in Path('/proc/meminfo').read_text().splitlines():
            key, value = line.split(':', 1)
            values[key] = int(value.strip().split()[0]) * 1024
        memory = values.get('MemAvailable', values['MemFree'])
        commit = max(0, values['CommitLimit'] - values['Committed_AS'])
        volume = str(parent.stat().st_dev)
    return dict(volume=volume, freeBytes=usage.free, availableMemoryBytes=memory, availableCommitBytes=commit, reservesSpace=False, modifiesPagefile=False)


def windows_argv(command):
    count = ctypes.c_int()
    shell = ctypes.windll.shell32
    shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
    pointer = shell.CommandLineToArgvW(command, ctypes.byref(count))
    a.require(bool(pointer), 'Build owner command-line parsing failed')
    try:
        return [pointer[i] for i in range(count.value)]
    finally:
        ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]
        ctypes.windll.kernel32.LocalFree(ctypes.cast(pointer, ctypes.c_void_p))


def active_owners():
    if os.name == 'nt':
        code = "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);@(Get-CimInstance Win32_Process -Filter \"Name='ninja.exe' OR Name='cmake.exe' OR Name='ctest.exe'\" -ErrorAction Stop | ForEach-Object { [ordered]@{pid=[int]$_.ProcessId;parent=[int]$_.ParentProcessId;command=[string]$_.CommandLine;birth=$_.CreationDate.ToUniversalTime().ToString('o')} })|ConvertTo-Json -Compress"
        result = subprocess.run(['pwsh', '-NoProfile', '-NonInteractive', '-Command', code], capture_output=True, encoding='utf-8', timeout=30, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
        rows = json.loads(result.stdout.strip() or '[]') or []
        if isinstance(rows, dict):
            rows = [rows]
        for row in rows:
            a.require(row['command'] and row['birth'], 'Build owner inventory is incomplete')
            row['argv'] = windows_argv(row['command'])
    else:
        rows = []
        for file in Path('/proc').glob('[0-9]*/cmdline'):
            try:
                argv = [os.fsdecode(v) for v in file.read_bytes().split(b'\0') if v]
                if argv and Path(argv[0]).name in ('ninja', 'cmake', 'ctest'):
                    rows.append(dict(pid=int(file.parent.name), argv=argv, cwd=str((file.parent / 'cwd').resolve()), parent=0))
            except (FileNotFoundError, ProcessLookupError):
                pass
    by_pid = {row['pid']: row for row in rows}
    for row in rows:
        hints = []
        argv = row['argv']
        if any(flag in argv for flag in ('--version', '--help', '-version')):
            row['readOnly'] = True
            continue
        for index, value in enumerate(argv):
            if value in ('--build', '--install', '--test-dir', '-B', '-C') and index + 1 < len(argv):
                hints.append(argv[index + 1])
            elif value.startswith(('-B', '-C')) and len(value) > 2:
                hints.append(value[2:])
        if not hints and row.get('cwd'):
            hints.append(row['cwd'])
        row['roots'] = [str(a.plain_path(value, exists=False)) for value in hints if Path(value).is_absolute()]
    owners = []
    for row in rows:
        if row.get('readOnly'):
            continue
        roots = list(row.get('roots', []))
        parent, visited = by_pid.get(row['parent']), set()
        while parent and parent['pid'] not in visited:
            visited.add(parent['pid'])
            roots.extend(parent.get('roots', []))
            parent = by_pid.get(parent['parent'])
        owners.append(dict(pid=row['pid'], birth=row.get('birth'), roots=sorted(set(roots)), unknownWorkspace=not roots))
    return owners


def positive(value, name):
    a.require(isinstance(value, int) and not isinstance(value, bool) and value > 0, name + ' must be a positive integer')
    return value


def owner_roots(owner):
    roots = [a.plain_path(value, exists=False) for value in owner.get('roots', [])]
    for root in list(roots):
        for parent in (root, *root.parents):
            if (parent / '.gn').is_file():
                roots.append(parent)
                break
    return roots


def reject_evidence_ancestors(root):
    for parent in (root, *root.parents):
        a.require(parent.name.casefold() not in ('logs', 'browser-profile') and not any((parent / name).is_file() for name in ('summary.json', 'capture-options.json', 'export.summary.json', 'capture.reference.json', 'package-manifest.json')), 'Capture/log/product ancestry is not a writable build/output workspace')


def writable_path_capability(root, role):
    reserved = {'con', 'prn', 'aux', 'nul', 'conin$', 'conout$'} | {prefix + str(n) for prefix in ('com', 'lpt') for n in range(1, 10)}
    for part in root.parts[1:]:
        a.require(not any(ord(c) < 32 or c in ':*?"<>|' for c in part) and not part.endswith(('.', ' ')) and part.split('.', 1)[0].casefold() not in reserved, 'Unsupported Windows writable path component')
    limit = 80 if role in ('checkout', 'native-build') else 160
    return len(str(root).encode('utf-16le')) // 2 <= limit


def check_mutable_root(root, role, edition, *, fresh=False, source=None):
    if not root.exists():
        return
    a.require(root.is_dir(), 'Workspace root is not an ordinary directory')
    a.require(not any((root / name).exists() for name in ('summary.json', 'capture-options.json', 'export.summary.json', 'capture.reference.json', 'package-manifest.json')), 'Evidence/product directory is not a mutable build workspace')
    marker = root / MARKER
    if marker.exists():
        value = a.read_json(marker)
        a.require(value.get('protocol') == 'v8trace/workspace-root/1' and value.get('role') == role and value.get('edition') == edition, 'Mixed edition or role in writable workspace')
        if source:
            a.require(a.plain_path(value['source']) == source, 'Existing workspace source differs')
    elif fresh:
        a.require(False, 'Explicit workspace preparation requires a fresh root')
    else:
        a.require(role == 'host-build' and source is not None and (root / 'CMakeCache.txt').is_file(), 'Unowned existing workspace is not adopted')
        cache = a.cmake_cache(root)
        a.require(cache.get('V8TRACE_EDITION') == edition and a.plain_path(cache.get('CMAKE_HOME_DIRECTORY', '')) == source / 'runtime-cpp', 'Legacy cache source/edition differs')
    a.inventory(root, independent=True)


def plan(request, *, probe=resources, owners=active_owners):
    a.require(request.get('protocol') == 'v8trace/workspace-request/1' and request.get('copyMode', 'independent') == 'independent', 'Separate writable roots require independent files, never hardlinks')
    a.require(set(request) <= {'protocol', 'workspaceId', 'roots', 'dependencies', 'copyMode', 'jobs', 'requiredAvailableMemoryBytes', 'requiredAvailableCommitBytes', 'protectedRoots'}, 'Unknown workspace request option')
    roots = request.get('roots')
    a.require(isinstance(roots, list) and 1 <= len(roots) <= 16, 'Explicit workspace roots are required')
    jobs = positive(request.get('jobs', 1), 'jobs')
    a.require(jobs <= 4, 'Workspace plan job budget is 1 through 4')
    memory = positive(request.get('requiredAvailableMemoryBytes', (2 + jobs * 2) * GIB), 'requiredAvailableMemoryBytes')
    commit = positive(request.get('requiredAvailableCommitBytes', (4 + jobs * 4) * GIB), 'requiredAvailableCommitBytes')
    dependencies_file = a.check_reference(request['dependencies'])
    dependencies = a.read_json(dependencies_file)
    a.assert_dependencies(dependencies)
    protected = [a.plain_path(value, exists=False) for value in request.get('protectedRoots', [])]
    pinned = [a.plain_path(value['root']) for value in dependencies['dependencies'].values()]
    observed_owners = owners()
    blockers, resolved, volumes = [], [], {}
    for row in roots:
        a.require(set(row) <= {'path', 'role', 'edition', 'requiredBytes', 'source'}, 'Unknown mutable root option')
        a.require(row.get('role') in ('checkout', 'native-build', 'host-build', 'package') and row.get('edition') in ('community', 'professional'), 'Workspace role/edition is required')
        root = a.plain_path(row['path'], exists=False)
        reject_evidence_ancestors(root)
        if not writable_path_capability(root, row['role']):
            blockers.append('writable-root-path-budget-not-verified')
        required = positive(row.get('requiredBytes'), 'requiredBytes')
        a.require(all(not overlaps(root, other) for other in [Path(item['path']) for item in resolved] + protected + pinned), 'Aliased/overlapping mutable, protected or read-only dependency roots')
        source = a.plain_path(row['source']) if row.get('source') else None
        a.require(row['role'] != 'host-build' or source is not None, 'Host workspace source identity is required')
        check_mutable_root(root, row['role'], row['edition'], fresh=True, source=source)
        state = probe(root)
        volume = volumes.setdefault(state['volume'], dict(freeBytes=state['freeBytes'], requiredBytes=0))
        volume['freeBytes'] = min(volume['freeBytes'], state['freeBytes'])
        volume['requiredBytes'] += required
        if state['availableMemoryBytes'] < memory:
            blockers.append('available-memory-below-request')
        if state['availableCommitBytes'] < commit:
            blockers.append('available-commit-below-request')
        for owner in observed_owners:
            if owner.get('unknownWorkspace') or any(overlaps(root, value) for value in owner_roots(owner)):
                blockers.append('active-or-unattributed-build-owner:' + str(owner['pid']))
        resolved.append(dict(path=str(root), role=row['role'], edition=row['edition'], requiredBytes=required, source=str(source) if source else None, resources=state))
    if any(value['requiredBytes'] > value['freeBytes'] for value in volumes.values()):
        blockers.append('insufficient-independent-workspace-capacity')
    value = dict(protocol='v8trace/workspace-plan/1', workspaceId=request.get('workspaceId') or uuid.uuid4().hex, request=request, roots=resolved,
                 owners=observed_owners, volumes=volumes, blockers=sorted(set(blockers)), readyToPrepare=not blockers, dependencies=request['dependencies'],
                 dependencyPolicy='pinned read-only input use, before/after verification; no ACL or mount changes', copyMode='independent',
                 migratesActiveBuild=False, createsEditionCheckout=False, modifiesPagefile=False, resourceReservation=False, remotePublication=False)
    value['planDigest'] = a.object_digest(value)
    return value


class DirectoryGuards:
    def __init__(self):
        self.handles = []

    def lock(self, path):
        a.require(os.name == 'nt', 'Workspace preparation currently requires Windows no-reparse directory handles')
        kernel = ctypes.windll.kernel32
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel.GetFileInformationByHandleEx.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong]
        class Attributes(ctypes.Structure):
            _fields_ = [('attributes', ctypes.c_ulong), ('tag', ctypes.c_ulong)]
        for directory in reversed((path, *path.parents)):
            handle = kernel.CreateFileW(str(directory), 0x80, 1 if directory == path else 3, None, 3, 0x02000000 | 0x00200000, None)
            a.require(handle not in (None, ctypes.c_void_p(-1).value), 'Workspace directory guard failed')
            self.handles.append(handle)
            info = Attributes()
            a.require(kernel.GetFileInformationByHandleEx(handle, 9, ctypes.byref(info), ctypes.sizeof(info)) and info.attributes & 0x10 and not info.attributes & 0x400, 'Workspace reparse/non-directory component rejected')

    def close(self):
        for handle in reversed(self.handles):
            ctypes.windll.kernel32.CloseHandle(handle)
        self.handles.clear()


def prepare(plan_file, *, probe=resources, owners=active_owners):
    value = a.read_json(plan_file)
    payload = dict(value)
    digest = payload.pop('planDigest', None)
    a.require(value.get('protocol') == 'v8trace/workspace-plan/1' and digest == a.object_digest(payload), 'Changed workspace plan')
    request = dict(value['request'], workspaceId=value['workspaceId'])
    current = plan(request, probe=probe, owners=owners)
    a.require(current['readyToPrepare'], 'Workspace capacity/ownership preflight has blockers: ' + ','.join(current['blockers']))
    guards, created = DirectoryGuards(), []
    try:
        for row in current['roots']:
            root = Path(row['path'])
            if root.exists():
                marker = a.read_json(root / MARKER)
                a.require(marker.get('workspaceId') == value['workspaceId'] and marker.get('planDigest') == digest, 'Existing prepared root belongs to another plan')
                a.require(list(root.iterdir()) == [root / MARKER], 'A populated workspace is not re-prepared')
                continue
            parent = root.parent
            a.require(parent.is_dir(), 'Prepare explicit existing parents first; planner does not create unrelated parent trees')
            guards.lock(parent)
            root.mkdir()
            created.append(str(root))
            guards.lock(root)
            marker = dict(protocol='v8trace/workspace-root/1', workspaceId=value['workspaceId'], planDigest=digest, role=row['role'], edition=row['edition'], source=row['source'],
                          dependencies=value['dependencies'], copyMode='independent', writableHardlinksForbidden=True, state='prepared-empty; no checkout migrated')
            a.write_new(root / MARKER, marker)
        return dict(protocol='v8trace/workspace-preparation/1', plan=a.reference(plan_file), workspaceId=value['workspaceId'], createdRoots=created,
                    state='prepared-empty-workspaces', sourceTreesCopied=False, hardlinksCreated=False, modifiesPagefile=False)
    except Exception:
        if created:
            print(json.dumps(dict(partialOwnedRootsRetained=created, activeBuildUntouched=True)))
        raise
    finally:
        guards.close()


def init_host(root, source, edition, jobs, required_bytes=GIB, *, probe=resources, owners=active_owners):
    root, source = a.plain_path(root, exists=False), a.plain_path(source)
    reject_evidence_ancestors(root)
    a.require(writable_path_capability(root, 'host-build'), 'Host root exceeds verified writable path budget')
    jobs = positive(jobs, 'jobs')
    a.require(edition in ('community', 'professional') and 1 <= jobs <= 4, 'Invalid host workspace edition/job budget')
    a.require((source / 'runtime-cpp/CMakeLists.txt').is_file(), 'Expected actual host source root')
    observed = probe(root)
    a.require(observed['freeBytes'] >= required_bytes and observed['availableMemoryBytes'] >= (2 + jobs * 2) * GIB and observed['availableCommitBytes'] >= (4 + jobs * 4) * GIB, 'Host build capacity/memory preflight failed; no settings or applications changed')
    for owner in owners():
        a.require(not owner.get('unknownWorkspace') and not any(overlaps(root, value) for value in owner_roots(owner)), 'An active/unattributed build owner prevents this host workspace')
    check_mutable_root(root, 'host-build', edition, source=source)
    if not root.exists():
        guards = DirectoryGuards()
        try:
            missing, parent = [], root
            while not parent.exists():
                missing.append(parent)
                parent = parent.parent
            guards.lock(parent)
            for directory in reversed(missing):
                directory.mkdir()
                guards.lock(directory)
            a.write_new(root / MARKER, dict(protocol='v8trace/workspace-root/1', workspaceId=uuid.uuid4().hex, role='host-build', edition=edition, source=str(source), state='prepared-empty; build not started', writableHardlinksForbidden=True))
        finally:
            guards.close()
    return dict(protocol='v8trace/host-workspace-preflight/1', root=str(root), edition=edition, source=str(source), resources=observed, jobs=jobs, startsBuild=False, modifiesPagefile=False)


def check_output(root, source, required_bytes, *, probe=resources, owners=active_owners):
    root, source = a.plain_path(root, exists=False), a.plain_path(source)
    reject_evidence_ancestors(root)
    a.require(writable_path_capability(root, 'package'), 'Package root exceeds verified writable path budget')
    a.require(not root.exists() and not any(overlaps(root, source / name) for name in ('ui', 'native', 'professional', 'runtime-cpp', 'editions')), 'A fresh output outside source component trees is required')
    capacity = probe(root)
    a.require(capacity['freeBytes'] >= positive(required_bytes, 'requiredBytes'), 'Independent package capacity is insufficient')
    for owner in owners():
        a.require(not owner.get('unknownWorkspace') and not any(overlaps(root, path) for path in owner_roots(owner)), 'An active checkout/output owner prevents this package destination')
    return dict(protocol='v8trace/package-output-preflight/1', output=str(root), resources=capacity, requiredBytes=required_bytes, createsOutput=False)


def check_consumer(plan_file, root, edition, source):
    value = a.read_json(plan_file)
    payload = dict(value)
    digest = payload.pop('planDigest', None)
    a.require(digest == a.object_digest(payload), 'Changed consumer workspace plan')
    root = a.plain_path(root)
    matches = [row for row in value['roots'] if Path(row['path']) == root and row['role'] == 'host-build' and row['edition'] == edition]
    a.require(len(matches) == 1, 'Host consumer root/edition is not selected by the plan')
    marker = a.read_json(root / MARKER)
    a.require(marker.get('workspaceId') == value['workspaceId'] and marker.get('planDigest') == digest, 'Unprepared or mixed workspace plan root')
    a.assert_dependencies(a.read_json(a.check_reference(value['dependencies'])))
    return init_host(root, source, edition, value['request'].get('jobs', 1))


def main():
    parser = argparse.ArgumentParser(description='Observe capacity/owners and prepare independent empty workspaces; never clone or migrate active builds.')
    parser.add_argument('command', choices=('plan', 'prepare', 'init-host', 'check-consumer', 'check-output'))
    for name in ('request', 'plan', 'root', 'source', 'output'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--edition', choices=('community', 'professional'))
    parser.add_argument('--jobs', type=int, default=1)
    parser.add_argument('--required-bytes', type=int, default=GIB)
    args = parser.parse_args()
    if args.command == 'plan': value = plan(a.read_json(args.request))
    elif args.command == 'prepare': value = prepare(args.plan)
    elif args.command == 'init-host': value = init_host(args.root, args.source, args.edition, args.jobs)
    elif args.command == 'check-output': value = check_output(args.root, args.source, args.required_bytes)
    else: value = check_consumer(args.plan, args.root, args.edition, args.source)
    if args.output:
        file = a.write_new(args.output, value)
        print(json.dumps(dict(receipt=str(file), sha256=a.file_digest(file), state=value.get('state'), readyToPrepare=value.get('readyToPrepare'), startsBuild=False)))
    else:
        print(json.dumps(value))


if __name__ == '__main__':
    main()
