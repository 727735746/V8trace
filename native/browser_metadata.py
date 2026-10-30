import datetime
import hashlib
import importlib.util
import json
import mmap
from pathlib import Path, PurePosixPath


spec = importlib.util.spec_from_file_location('native_media_build', Path(__file__).with_name('media_build.py'))
media_build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media_build)


def digest(file):
    value = hashlib.sha256()
    with file.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1048576), b''):
            value.update(chunk)
    return value.hexdigest()


def load(file):
    return json.loads(file.read_text('utf-8-sig'))


def write(file, value):
    file.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def write_metadata(output, projection_file, receipt_file, dependencies_file):
    projection, receipt = load(projection_file), load(receipt_file)
    if (projection.get('edition') != 'community' or projection.get('professionalProducersIncluded') is not False
            or receipt['status'] != 'compiled-not-validated' or receipt['exitCode'] != 0
            or receipt['releaseDefaults']['edition'] != 'community'
            or receipt['releaseDefaults']['sourceProjectionSha256'] != digest(projection_file)):
        raise RuntimeError('A source-bound successful Community build is required')
    compiled = {row['path']: row['sha256'] for row in receipt['sources']}
    if not projection.get('files'):
        raise RuntimeError('Community source manifest is empty')
    for row in projection['files']:
        if compiled.get(row['path']) != row['sha256']:
            raise RuntimeError('Compiled shared source differs: ' + row['path'])
    args_hash = receipt['releaseDefaults'].get('gnArgsSha256')
    if receipt['releaseDefaults'].get('fieldTrialTestingConfig') is not False or args_hash != digest(output / 'args.gn'):
        raise RuntimeError('Compiled GN arguments changed')
    media = media_build.verify(output / 'args.gn')
    if receipt['releaseDefaults'].get('mediaBuild') != media:
        raise RuntimeError('Compiled media configuration differs')
    core = {row['path']: row['sha256'] for row in receipt['binaries']}
    if set(core) != {'chrome.exe', 'chrome.dll', 'chrome_elf.dll', 'v8_context_snapshot.bin'}:
        raise RuntimeError('Incomplete native binary evidence')
    for name, expected in core.items():
        if digest(output / name) != expected:
            raise RuntimeError('Compiled browser changed: ' + name)
    forbidden = ['V8TraceOpcode', 'V8TraceWasm', 'V8TraceCausality', '"vm-state"']
    with (output / 'chrome.dll').open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as binary:
        if binary.find(b'v8trace/browser-edition/1') < 0:
            raise RuntimeError('Compiled browser has no Community producer identity')
        for token in forbidden:
            if binary.find(token.encode()) >= 0 or binary.find(token.encode('utf-16-le')) >= 0:
                raise RuntimeError('Professional producer remains in Community binary: ' + token)
    root_hash, v8_hash = projection['patchHashes']['chromium'], projection['patchHashes']['v8']
    for role, expected in projection['patchHashes'].items():
        if digest(projection_file.parent / ('community-' + role + '.patch')) != expected:
            raise RuntimeError('Projected public patch changed')
    dependencies = load(dependencies_file)['//chrome:chrome']['runtime_deps']
    files = set(core)
    for entry in dependencies:
        relative = PurePosixPath(entry.replace('\\', '/'))
        if relative.suffix.lower() == '.pdb' or relative.parts[0] == 'initialexe':
            continue
        if relative.is_absolute() or '..' in relative.parts or ':' in str(relative):
            raise RuntimeError('GN runtime dependency leaves output: ' + entry)
        file = output.joinpath(*relative.parts)
        if not file.resolve(strict=True).is_relative_to(output) or not file.is_file():
            raise RuntimeError('Invalid GN runtime dependency: ' + entry)
        for path in [file, *file.parents]:
            if path == output:
                break
            if path.lstat().st_file_attributes & 1024:
                raise RuntimeError('Reparse point in runtime dependency: ' + entry)
        files.add(relative.as_posix())
    if not 5 <= len(files) <= 100000:
        raise RuntimeError('Invalid Community runtime inventory size')
    descriptor = dict(schemaVersion=1, backend='chromium-opcode', protocol='v8trace/opcode/1',
        category='disabled-by-default-v8.opcode', edition='community', editionProtocol='v8trace/edition/1',
        producerEditionProtocol='v8trace/browser-edition/1', chromiumRevision=projection['chromiumRevision'],
        v8Revision=projection['v8Revision'], patchSha256=v8_hash, sourcePatchHashes=projection['patchHashes'],
        executableSha256=core['chrome.exe'], binaryHashes=core,
        builtAt=datetime.datetime.now(datetime.timezone.utc).isoformat(), opcodeSupported=False,
        professionalProducersIncluded=False, nativeRunValidated=False,
        buildEvidenceProtocol='v8trace/build-supervisor/1', buildAttemptId=receipt['attemptId'],
        buildReceiptSha256=digest(receipt_file), sourceProjectionSha256=digest(projection_file),
        buildResourceProfile=receipt['releaseDefaults'])
    descriptor['mediaBuild'] = media
    protocols = dict(startup='lpac-startup/1', imeCompat='ime-compat/1', language='languages/1',
        startupDefaults='startup-defaults/1', detail='detail/1', objectSnapshot='objects/1',
        detailFilter='detail-filter/1', detailHotPath='detail-hotpath/1', call='call/2',
        functionDictionary='function/1', callStats='call-stats/1', callValueFilter='detail-filter/2',
        envProbe='env/1', envStats='env-stats/1', envBuiltin='env-builtin/1', closure='closure/1',
        state='state/1', stateStats='state-stats/1', identity='identity/1', aux='aux/1',
        bytecodeIdentity='bytecode-identity/1', browser='browser/1', redirectBody='redirect-body/1',
        recordingControl='recording/1', traceTransport='trace-transport/1')
    descriptor.update({key + 'Protocol': 'v8trace/' + value for key, value in protocols.items()})
    for key in ['detailFilter', 'envProbe', 'closure', 'identity', 'aux']:
        descriptor[key + 'PatchSha256'] = v8_hash
    for key in ['state', 'browser']:
        descriptor[key + 'PatchSha256'] = root_hash
    for key in ['startup', 'imeCompat', 'language', 'objectSwitch', 'recordingControl']:
        descriptor[key + 'PatchSha256'] = root_hash
    descriptor.update(detailFeatures=['jscall', 'native'], envProbeFeatures=['absent', 'interfaces', 'mutations', 'inventory'],
        stateFeatures=['websocket', 'indexeddb', 'cookie', 'storage', 'wasm-module'],
        browserFeatures=['env', 'http', 'script', 'console', 'exception'],
        preferredLanguages=['en-US', 'en'], applicationLocale='en-US', fieldTrialTestingConfig=False,
        gnArgsSha256=digest(output / 'args.gn'), apiKeyInfoBarSuppressed=True,
        imeCompatScope='tsf-native-calls;thread-policy-restored;process-policy-preserved',
        recordingControlTransport='local-files-no-cdp', traceSharedMemoryMaxBytes=8388608)
    descriptor_file = output / 'v8trace-opcode.json'
    inventory_file = output / 'v8trace-runtime-files.json'
    if descriptor_file.exists() or inventory_file.exists():
        raise RuntimeError('Browser metadata already exists; keep the prior evidence')
    write(descriptor_file, descriptor)
    files.update(['v8trace-opcode.json', 'v8trace-runtime-files.json'])
    write(inventory_file, dict(schemaVersion=1, protocol='v8trace/runtime-files/1', source='GN runtime_deps',
        descriptorSha256=digest(descriptor_file), files=sorted(files)))
    result = dict(protocol='v8trace/community-browser-package/1', status='compiled-awaiting-native-validation',
        edition='community', executable=str(output / 'chrome.exe'), files=len(files), binaryHashes=core,
        descriptorSha256=digest(descriptor_file), inventorySha256=digest(inventory_file),
        privateProducerBinaryScan='PASS', sourceProjectionSha256=digest(projection_file),
        nativeRunValidated=False)
    return result
