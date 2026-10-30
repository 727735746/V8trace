import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import xml.etree.ElementTree as ET
import zipfile


LIMIT = 100000
DEPENDENCIES = ('json', 'zlib', 'brotli', 'zstd', 'ixwebsocket', 'sqlite', 'abseil', 're2')


def require(value, message):
    if not value:
        raise ValueError(message)


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8', 'backslashreplace')


def object_digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def plain_path(value, *, exists=True):
    value = Path(value)
    require('..' not in value.parts and '\0' not in str(value), 'Parent traversal or NUL path rejected')
    value = Path(os.path.abspath(value))
    for part in reversed((value, *value.parents)):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        require(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400, 'Reparse/symlink path rejected: ' + str(part))
    if exists:
        require(value.exists(), 'Missing binding input: ' + str(value))
    return value.resolve(strict=exists)


def file_digest(file):
    file = plain_path(file)
    require(file.is_file(), 'A regular file is required: ' + str(file))
    result = hashlib.sha256()
    before = file.stat()
    with file.open('rb') as stream:
        for data in iter(lambda: stream.read(1048576), b''):
            result.update(data)
        opened = os.fstat(stream.fileno())
    after = file.stat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
            and (opened.st_dev, opened.st_ino) == (after.st_dev, after.st_ino), 'Input changed while hashing: ' + str(file))
    return result.hexdigest()


def read_json(file):
    file = plain_path(file)
    require(file.stat().st_size <= 67108864, 'JSON binding exceeds 64 MiB')
    return json.loads(file.read_text(encoding='utf-8-sig'))


def write_new(file, value):
    file = plain_path(file, exists=False)
    require(file.parent.is_dir() and not file.exists(), 'A new receipt under an existing ordinary parent is required')
    with file.open('xb') as stream:
        stream.write(json.dumps(value, ensure_ascii=False, indent=2).encode('utf-8', 'backslashreplace') + b'\n')
        stream.flush()
        os.fsync(stream.fileno())
    return file


def reference(file):
    file = plain_path(file)
    return dict(path=str(file), bytes=file.stat().st_size, sha256=file_digest(file))


def check_reference(row):
    actual = reference(row['path'])
    require(actual == row, 'Changed bound file: ' + row['path'])
    return Path(row['path'])


def relative_name(value):
    require(isinstance(value, str) and value and '\0' not in value and '\\' not in value and ':' not in value, 'Invalid portable relative binding path')
    name = PurePosixPath(value)
    require(not name.is_absolute() and all(part not in ('', '.', '..') for part in value.split('/')), 'Binding path traversal rejected')
    return name.as_posix()


def inventory(root, *, independent=False):
    root = plain_path(root)
    require(root.is_dir(), 'Inventory root must be a directory')
    files, names, pending, entries, name_bytes = [], set(), [root], 0, 0
    while pending:
        for item in pending.pop().iterdir():
            entries += 1
            require(entries <= LIMIT, 'Inventory exceeds explicit entry limit')
            item = plain_path(item)
            if item.is_dir():
                pending.append(item)
            else:
                require(item.is_file(), 'Non-regular inventory entry rejected')
                name = relative_name(item.relative_to(root).as_posix())
                require(name.casefold() not in names, 'Case-aliased inventory paths rejected')
                names.add(name.casefold())
                name_bytes += len(name.encode('utf-8'))
                require(name_bytes <= 33554432, 'Inventory path bytes exceed budget')
                if independent:
                    require(item.stat().st_nlink == 1, 'Writable product/workspace hardlink rejected: ' + name)
                files.append(dict(path=name, bytes=item.stat().st_size, sha256=file_digest(item)))
    return sorted(files, key=lambda row: row['path'])


def source_snapshot(root, edition):
    root = plain_path(root)
    require(edition in ('community', 'professional'), 'Unknown edition')
    require((root / 'runtime-cpp/CMakeLists.txt').is_file(), 'Expected V8 Trace source root')
    if edition == 'professional':
        require((root / 'professional/runtime/sources.cmake').is_file(), 'Professional implementation is absent')
    grouped = {}
    selections = [('runtime', 'runtime-cpp/src'), ('runtime', 'runtime-cpp/include'), ('runtime', 'runtime-cpp/cmake'),
                  ('tests', 'runtime-cpp/tests'), ('ui', 'ui'), ('native-inputs', 'native'), ('packaging', 'editions')]
    if edition == 'professional':
        selections.append(('runtime', 'professional/runtime'))
    suffixes = {'.cpp', '.cc', '.c', '.h', '.hpp', '.rc', '.in', '.py', '.cs', '.ps1', '.cmake', '.json', '.md', '.txt', '.ico', '.png', '.manifest', '.config', '.patch'}
    for group, directory in selections:
        folder = root / directory
        if not folder.is_dir():
            continue
        pending = [folder]
        selected = []
        while pending:
            directory = pending.pop()
            for file in directory.iterdir():
                file = plain_path(file)
                if file.is_dir():
                    if file.name not in ('.cache', 'out', 'dist', '.git', '__pycache__') and not (group == 'packaging'):
                        pending.append(file)
                elif file.suffix.lower() in suffixes:
                    selected.append(file)
                require(len(selected) <= LIMIT, 'Source input count exceeds budget')
        for file in selected:
            if file.suffix.lower() in suffixes:
                if group == 'packaging' and len(file.relative_to(folder).parts) != 1:
                    continue
                row = dict(path=file.relative_to(root).as_posix(), bytes=file.stat().st_size, sha256=file_digest(file))
                grouped.setdefault(group, []).append(row)
    for name, group in [('runtime-cpp/CMakeLists.txt', 'runtime'), ('hooks.js', 'assets'), ('scenario_presets.json', 'assets'),
                        ('case_prompt_templates.json', 'assets'), ('build-cpp.ps1', 'packaging'), ('build.ps1', 'packaging'),
                        ('编译指令.txt', 'packaging'), ('runtime-cpp/licenses/sqlite.txt', 'packaging'),
                        ('editions/community/LICENSE', 'packaging'), ('editions/community/NOTICE', 'packaging')]:
        file = root / name
        if file.is_file():
            grouped.setdefault(group, []).append(dict(path=name, bytes=file.stat().st_size, sha256=file_digest(file)))
    if edition == 'professional':
        file = root / 'professional/NOTICE'
        require(file.is_file(), 'Professional package notice is absent')
        grouped.setdefault('packaging', []).append(dict(path='professional/NOTICE', bytes=file.stat().st_size, sha256=file_digest(file)))
    for file in list(root.glob('*.md')) + list((root / 'runtime-cpp').glob('*.md')):
        grouped.setdefault('packaging', []).append(dict(path=file.relative_to(root).as_posix(), bytes=file.stat().st_size, sha256=file_digest(file)))
    grouped = {key: sorted(rows, key=lambda row: row['path']) for key, rows in sorted(grouped.items())}
    runtime_inputs = {key: grouped.get(key, []) for key in ('runtime', 'tests', 'assets', 'native-inputs')}
    return dict(protocol='v8trace/source-inputs/1', edition=edition, root=str(root), groups=grouped,
                sourceDigest=object_digest(grouped), runtimeInputDigest=object_digest(runtime_inputs), uiInputDigest=object_digest(grouped.get('ui', [])))


def assert_source(snapshot, root=None, scope='all'):
    require(snapshot.get('protocol') == 'v8trace/source-inputs/1', 'Unknown source snapshot')
    groups = snapshot['groups']
    require(snapshot['sourceDigest'] == object_digest(groups) and snapshot['uiInputDigest'] == object_digest(groups.get('ui', []))
            and snapshot['runtimeInputDigest'] == object_digest({key: groups.get(key, []) for key in ('runtime', 'tests', 'assets', 'native-inputs')}), 'Source snapshot declared digests differ from its inputs')
    actual = source_snapshot(root or snapshot['root'], snapshot['edition'])
    key = 'runtimeInputDigest' if scope == 'runtime' else 'sourceDigest'
    require(actual[key] == snapshot[key], 'Source inputs changed after snapshot: ' + scope)
    return actual


def cmake_cache(root):
    values = {}
    for line in (plain_path(root) / 'CMakeCache.txt').read_text(encoding='utf-8-sig').splitlines():
        if '=' in line and not line.startswith(('#', '//')):
            key, value = line.split('=', 1)
            values[key.split(':', 1)[0]] = value
    return values


def dependency_snapshot(build=None, dependency_root=None, compiler=None):
    build = plain_path(build) if build else None
    cache = cmake_cache(build) if build else {}
    dependency_root = plain_path(dependency_root) if dependency_root else None
    roots = {}
    for name in DEPENDENCIES:
        raw = str(dependency_root / '_deps' / (name + '-src')) if dependency_root else cache.get('FETCHCONTENT_SOURCE_DIR_' + name.upper()) or str(build / '_deps' / (name + '-src'))
        root = plain_path(raw)
        files = inventory(root)
        roots[name] = dict(root=str(root), files=files, digest=object_digest(files), accessPolicy='read-only-source-use; verified before/after, no ACL or volume immutability claim')
    compiler = reference(compiler or cache['CMAKE_CXX_COMPILER'])
    return dict(protocol='v8trace/dependency-inputs/1', dependencies=roots, compiler=compiler, digest=object_digest(dict(roots=roots, compiler=compiler)))


def assert_dependencies(snapshot):
    require(snapshot.get('protocol') == 'v8trace/dependency-inputs/1', 'Unknown dependency snapshot')
    require(set(snapshot.get('dependencies', {})) == set(DEPENDENCIES), 'Pinned dependency set differs')
    check_reference(snapshot['compiler'])
    for item in snapshot['dependencies'].values():
        require(inventory(item['root']) == item['files'], 'Pinned read-only dependency changed: ' + item['root'])


def runtime_identity(runtime):
    before = reference(runtime)
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    result = subprocess.run([str(runtime), '--version'], capture_output=True, timeout=15, creationflags=flags)
    require(result.returncode == 0 and not result.stderr and len(result.stdout) <= 65536, 'Unexpected actual runtime identity response')
    require(reference(runtime) == before, 'Runtime changed during identity observation')
    return json.loads(result.stdout)


def junit_passed(file):
    file = plain_path(file)
    require(file.stat().st_size <= 16777216, 'JUnit report exceeds budget')
    data = file.read_bytes()
    require(b'<!DOCTYPE' not in data.upper() and b'<!ENTITY' not in data.upper(), 'JUnit entities rejected')
    root = ET.fromstring(data)
    cases = list(root.iter('testcase'))
    require(cases and all(not list(case.iter(tag)) for case in cases for tag in ('failure', 'error', 'skipped')), 'Acceptance did not execute every JUnit case successfully')
    require(int(root.get('tests', len(cases))) == len(cases), 'JUnit test count differs')
    for suite in root.iter('testsuite'):
        require(all(int(suite.get(key, 0)) == 0 for key in ('failures', 'errors', 'disabled', 'skipped')), 'JUnit contains failed/disabled cases')
    return len(cases)


def runtime_receipt(snapshot_file, dependencies_file, runtime, junit, *, observe=runtime_identity):
    source = read_json(snapshot_file)
    assert_source(source, scope='runtime')
    dependencies = read_json(dependencies_file)
    assert_dependencies(dependencies)
    count = junit_passed(junit)
    identity = observe(runtime)
    require(identity['edition']['id'] == source['edition'] and identity['edition']['protocol'] == 'v8trace/edition/1', 'Actual compiled runtime edition differs')
    return dict(protocol='v8trace/runtime-build-acceptance/1', edition=source['edition'], status='host-tests-passed', testCount=count,
                runtime=reference(runtime), identity=identity, sourceInputs=reference(snapshot_file), runtimeInputDigest=source['runtimeInputDigest'],
                dependencies=reference(dependencies_file), testReport=reference(junit), nativeBrowserValidated=False)


def verify_runtime_receipt(file, runtime, snapshot, *, current_inputs=False):
    value = read_json(file)
    require(value.get('protocol') == 'v8trace/runtime-build-acceptance/1' and value.get('status') == 'host-tests-passed', 'Bound runtime acceptance is required')
    require(value['edition'] == snapshot['edition'] and value['runtimeInputDigest'] == snapshot['runtimeInputDigest'], 'Runtime/source edition or tested inputs differ')
    require(reference(runtime)['sha256'] == value['runtime']['sha256'], 'Tested runtime bytes differ')
    for key in ('sourceInputs', 'dependencies', 'testReport'):
        check_reference(value[key])
    require(junit_passed(value['testReport']['path']) == value['testCount'], 'Bound test report differs')
    if current_inputs:
        check_reference(value['runtime'])
        assert_dependencies(read_json(value['dependencies']['path']))
    return value


def generated_hooks(root):
    value = (root / 'hooks.js').read_bytes().replace(b'\r\n', b'\n')
    begin, end = value.index(b'function installDiagnosticHooks('), value.index(b'\nfunction createHookSource(')
    require(end > begin, 'Unexpected diagnostic hook generation boundary')
    generated = value[begin:end]
    return generated.replace(b'\n', b'\r\n') if os.name == 'nt' else generated


def native_input(root, edition):
    root = plain_path(root)
    descriptor_file, inventory_file = root / 'v8trace-opcode.json', root / 'v8trace-runtime-files.json'
    descriptor, declared = read_json(descriptor_file), read_json(inventory_file)
    require(descriptor.get('edition', 'professional') == edition and declared.get('protocol') == 'v8trace/runtime-files/1', 'Native edition/inventory differs')
    require(declared.get('descriptorSha256') == file_digest(descriptor_file), 'Native inventory descriptor binding differs')
    names = [relative_name(name.replace('\\', '/')) for name in declared['files']]
    require(len(names) == len(set(name.casefold() for name in names)), 'Aliased native inventory names')
    require({'chrome.exe', 'chrome.dll', 'v8trace-opcode.json'} <= set(names), 'Native core inventory is incomplete')
    files = inventory(root)
    require({row['path'] for row in files} == set(names) | {'v8trace-runtime-files.json'}, 'Native runtime inventory does not exactly cover bundle')
    hashes = descriptor['binaryHashes']
    require({'chrome.exe', 'chrome.dll'} <= hashes.keys(), 'Descriptor lacks native core hashes')
    for name, digest in hashes.items():
        name = relative_name(name.replace('\\', '/'))
        require(name in names and file_digest(root / name) == digest, 'Changed descriptor-bound native core: ' + name)
    return dict(protocol='v8trace/native-inputs/1', root=str(root), files=files, descriptor=reference(descriptor_file), inventory=reference(inventory_file), coreHashes=hashes, descriptorData=descriptor)


def native_components(package, edition):
    root = package / 'chromium'
    return native_input(root, edition) if root.exists() else None


def make_binding(package, snapshot_file, runtime_acceptance, batch, compiler, native_inputs=None, *, observe=runtime_identity):
    package = plain_path(package)
    snapshot = read_json(snapshot_file)
    root = Path(snapshot['root'])
    assert_source(snapshot)
    manifest_file = package / 'package-manifest.json'
    manifest = read_json(manifest_file)
    require(manifest.get('editionProtocol') == 'v8trace/edition/1' and manifest.get('runtime') == 'cpp' and manifest.get('nodeRequired') is False and manifest.get('automaticRuntimeFallback') is False, 'Unexpected package runtime/edition contract')
    require(manifest['edition'] == snapshot['edition'] and manifest.get('packageBatchId') == batch and batch, 'Package batch/edition differs')
    require(manifest.get('artifactBindingProtocol') == 'v8trace/product-binding/1' and manifest.get('sourceInputsSha256') == file_digest(snapshot_file)
            and manifest.get('sourceDigest') == snapshot['sourceDigest'] and manifest.get('uiInputDigest') == snapshot['uiInputDigest']
            and manifest.get('runtimeInputDigest') == snapshot['runtimeInputDigest'] and manifest.get('runtimeAcceptanceSha256') == file_digest(runtime_acceptance), 'Package manifest source/UI/runtime receipt metadata differs')
    files = inventory(package, independent=True)
    listed = [dict(path=relative_name(row['path'].replace('\\', '/')), bytes=row['bytes'], sha256=row['sha256']) for row in manifest['files']]
    require(sorted(listed, key=lambda row: row['path']) == [row for row in files if row['path'] != 'package-manifest.json'], 'Package manifest does not exactly bind all files')
    runtime = package / 'runtime/v8trace-runtime.exe'
    accepted = verify_runtime_receipt(runtime_acceptance, runtime, snapshot, current_inputs=True)
    identity = observe(runtime)
    require(identity == accepted['identity'], 'Packaged runtime identity differs from tested runtime')
    require(manifest.get('runtimeVersion') == identity.get('version'), 'Declared runtime component version differs')
    require(manifest.get('uiCompilerSha256') == file_digest(compiler), 'UI compiler changed between build and binding')
    for name in ('scenario_presets.json', 'case_prompt_templates.json'):
        require(file_digest(root / name) == file_digest(package / 'app/v8-trace' / name), 'Packaged source asset differs: ' + name)
    require(hashlib.sha256(generated_hooks(root)).hexdigest() == file_digest(package / 'app/v8-trace/diagnostic-hooks.js'), 'Generated diagnostic hook bytes differ')
    native = native_components(package, snapshot['edition'])
    require(bool(native) == manifest['browserIncluded'], 'Browser inclusion differs from manifest')
    if native_inputs:
        prior = read_json(native_inputs)
        require(native and prior.get('protocol') == 'v8trace/native-inputs/1' and prior['files'] == native['files'] and prior['coreHashes'] == native['coreHashes'], 'Native source files changed between input observation and package assembly')
    value = dict(protocol='v8trace/product-binding/1', package=str(package), packageBatchId=batch, edition=snapshot['edition'], files=files,
                 manifest=reference(manifest_file), sourceInputs=reference(snapshot_file), sourceDigest=snapshot['sourceDigest'], uiInputDigest=snapshot['uiInputDigest'],
                 ui=reference(package / 'V8Trace.exe'), uiCompiler=reference(compiler), runtime=reference(runtime), runtimeIdentity=identity,
                 runtimeAcceptance=reference(runtime_acceptance), native=native, nativeInputObservation=reference(native_inputs) if native_inputs else None, componentVersions=dict(runtime=identity.get('version'), ui=manifest.get('uiVersion'), browser=(native or {}).get('descriptorData', {}).get('chromiumRevision')),
                 acceptanceState='assembled-byte-bound; final UI/native/media acceptance still required', remotePublication=False)
    value['bindingDigest'] = object_digest(value)
    return value


def verify_binding(file, *, source=False):
    value = read_json(file)
    payload = dict(value)
    recorded = payload.pop('bindingDigest', None)
    require(value.get('protocol') == 'v8trace/product-binding/1' and recorded == object_digest(payload), 'Changed product binding')
    package = plain_path(value['package'])
    require(inventory(package, independent=True) == value['files'], 'Changed bound product inventory')
    for key in ('manifest', 'sourceInputs', 'ui', 'runtime', 'runtimeAcceptance'):
        check_reference(value[key])
    snapshot = read_json(value['sourceInputs']['path'])
    require(snapshot['sourceDigest'] == value['sourceDigest'] and snapshot['uiInputDigest'] == value['uiInputDigest'], 'Changed source/UI inputs')
    verify_runtime_receipt(value['runtimeAcceptance']['path'], value['runtime']['path'], snapshot, current_inputs=bool(source))
    require(native_components(package, value['edition']) == value['native'], 'Changed native component/descriptor/inventory combination')
    if value.get('nativeInputObservation'):
        prior = read_json(check_reference(value['nativeInputObservation']))
        require(prior['files'] == value['native']['files'], 'Changed native source observation')
        if source:
            require(native_input(prior['root'], value['edition']) == prior, 'Native input source changed after observation')
    if source:
        check_reference(value['uiCompiler'])
        assert_source(snapshot, None if source is True else source)
    return value


def bind_acceptance(binding_file, report_file):
    product = verify_binding(binding_file, source=True)
    report = read_json(report_file)
    require(report.get('protocol') == 'v8trace/product-acceptance/1' and report.get('status') == 'passed', 'Explicit product acceptance envelope required')
    expected = dict(productBindingSha256=file_digest(binding_file), edition=product['edition'], packageBatchId=product['packageBatchId'], uiSha256=product['ui']['sha256'], runtimeSha256=product['runtime']['sha256'])
    require(all(report.get(key) == value for key, value in expected.items()), 'Acceptance product/component identity differs')
    require(report.get('scope') in ('host', 'native-media') and report.get('gates'), 'Acceptance scope/gates missing')
    observed = set()
    for gate in report['gates']:
        raw = read_json(check_reference(gate['report']))
        require(gate.get('status') == 'passed' and str(raw.get('status', raw.get('conclusion', raw.get('passed')))).lower() in ('passed', 'pass', 'true'), 'Actual acceptance gate did not pass')
        for key, component in (('uiSha256', 'ui'), ('runtimeSha256', 'runtime')):
            if key in raw:
                require(raw[key] == expected[key], 'Actual gate component identity differs: ' + key)
                observed.add(component)
        if 'edition' in raw:
            require(raw['edition'] == product['edition'], 'Actual gate edition differs')
        if 'nativeCoreHashes' in raw:
            require(product['native'] and raw['nativeCoreHashes'] == product['native']['coreHashes'] and raw.get('browserLaunched') is True, 'Actual native gate inputs differ')
            observed.add('native')
            if raw.get('mediaValidated') is True:
                observed.add('media')
    require({'ui', 'runtime'} <= observed, 'Actual UI/runtime hash observations are required')
    if report['scope'] == 'native-media':
        native = product['native']
        require({'native', 'media'} <= observed and native and report.get('nativeCoreHashes') == native['coreHashes'] and report.get('descriptorSha256') == native['descriptor']['sha256']
                and report.get('runtimeInventorySha256') == native['inventory']['sha256'] and report.get('browserLaunched') is True and report.get('mediaValidated') is True, 'Native/media acceptance inputs or observations missing')
    return dict(protocol='v8trace/bound-acceptance/1', product=reference(binding_file), report=reference(report_file), scope=report['scope'], gates=report['gates'], status='bound-observation; not independent test execution', remotePublication=False)


def verify_acceptance(file, binding_file):
    value = read_json(file)
    require(value.get('protocol') == 'v8trace/bound-acceptance/1' and value['product'] == reference(binding_file), 'Acceptance binding changed')
    actual = bind_acceptance(binding_file, check_reference(value['report']))
    require(actual == value, 'Acceptance receipt/gates changed')
    return value


def bind_archive(binding_file, archive, acceptance=None, prefix=''):
    product = verify_binding(binding_file)
    archive = plain_path(archive)
    if prefix:
        prefix = relative_name(prefix.rstrip('/')) + '/'
    expected = {row['path']: row for row in product['files']}
    seen = set()
    with zipfile.ZipFile(archive) as zipped:
        require(len(zipped.infolist()) <= LIMIT, 'Archive entry budget exceeded')
        for entry in zipped.infolist():
            name = relative_name(entry.filename.rstrip('/'))
            mode = stat.S_IFMT(entry.external_attr >> 16)
            require(entry.filename.startswith(prefix) and mode in (0, stat.S_IFREG, stat.S_IFDIR) and not entry.external_attr & 0x400, 'Archive prefix/link/type differs')
            require(mode in ((0, stat.S_IFDIR) if entry.is_dir() else (0, stat.S_IFREG)), 'Archive directory/file mode differs')
            if entry.is_dir():
                continue
            name = name[len(prefix):]
            require(name in expected and name.casefold() not in seen and entry.file_size == expected[name]['bytes'], 'Archive file inventory differs')
            seen.add(name.casefold())
            digest = hashlib.sha256()
            with zipped.open(entry) as stream:
                for data in iter(lambda: stream.read(1048576), b''):
                    digest.update(data)
            require(digest.hexdigest() == expected[name]['sha256'], 'Archive file bytes differ')
    require(seen == {name.casefold() for name in expected}, 'Archive missing bound files')
    if acceptance:
        verify_acceptance(acceptance, binding_file)
    return dict(protocol='v8trace/bound-archive/1', product=reference(binding_file), archive=reference(archive), archivePrefix=prefix,
                acceptance=reference(acceptance) if acceptance else None, packageBatchId=product['packageBatchId'], remotePublication=False, wholePackageReleaseEligible=False)


def verify_archive(file):
    value = read_json(file)
    require(value.get('protocol') == 'v8trace/bound-archive/1', 'Unknown bound archive')
    binding = check_reference(value['product'])
    archive = check_reference(value['archive'])
    acceptance = check_reference(value['acceptance']) if value['acceptance'] else None
    require(bind_archive(binding, archive, acceptance, value['archivePrefix']) == value, 'Archive/binding/acceptance combination changed')
    return value


def main():
    parser = argparse.ArgumentParser(description='Local immutable source/component/acceptance/archive binding; no publication.')
    parser.add_argument('command', choices=('snapshot', 'check-source', 'dependencies', 'check-dependencies', 'native-inputs', 'runtime-receipt', 'seal', 'check', 'acceptance', 'archive', 'check-archive'))
    for name in ('source', 'snapshot', 'dependencies', 'dependency-root', 'native-inputs', 'build', 'runtime', 'junit', 'package', 'runtime-acceptance', 'compiler', 'binding', 'report', 'archive', 'acceptance', 'output'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--edition', choices=('community', 'professional'))
    parser.add_argument('--scope', choices=('all', 'runtime'), default='all')
    parser.add_argument('--batch')
    parser.add_argument('--archive-prefix', default='')
    args = parser.parse_args()
    if args.command == 'snapshot': value = source_snapshot(args.source, args.edition)
    elif args.command == 'check-source': value = assert_source(read_json(args.snapshot), args.source, args.scope)
    elif args.command == 'dependencies': value = dependency_snapshot(args.build, args.dependency_root, args.compiler)
    elif args.command == 'check-dependencies': value = read_json(args.dependencies); assert_dependencies(value)
    elif args.command == 'native-inputs': value = native_input(args.source, args.edition)
    elif args.command == 'runtime-receipt': value = runtime_receipt(args.snapshot, args.dependencies, args.runtime, args.junit)
    elif args.command == 'seal': value = make_binding(args.package, args.snapshot, args.runtime_acceptance, args.batch, args.compiler, args.native_inputs)
    elif args.command == 'check': value = verify_binding(args.binding, source=args.source or False)
    elif args.command == 'acceptance': value = bind_acceptance(args.binding, args.report)
    elif args.command == 'archive': value = bind_archive(args.binding, args.archive, args.acceptance, args.archive_prefix)
    else: value = verify_archive(args.report)
    if args.output:
        if args.command == 'seal':
            require(not plain_path(args.output, exists=False).is_relative_to(plain_path(args.package)), 'Product binding must remain outside the sealed package to avoid inventory cycles')
        file = write_new(args.output, value)
        print(json.dumps(dict(protocol=value['protocol'], receipt=str(file), sha256=file_digest(file), remotePublication=False)))
    else:
        print(json.dumps(dict(protocol=value['protocol'], verified=True, remotePublication=False)))


if __name__ == '__main__':
    main()
