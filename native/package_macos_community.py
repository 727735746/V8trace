import argparse
import datetime
import hashlib
import json
import mmap
import os
from pathlib import Path
import subprocess
import sys


def digest(file):
    value = hashlib.sha256()
    with Path(file).open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            value.update(block)
    return value.hexdigest()


def write_json(file, value):
    file = Path(file)
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\n')


def git_revision(checkout, relative='.'):
    return subprocess.check_output(['git', '-C', str(Path(checkout) / relative), 'rev-parse', 'HEAD'], text=True).strip()


def contains(file, token):
    with Path(file).open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as binary:
        return binary.find(token.encode()) >= 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--out-dir', type=Path)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--validated-report', type=Path)
    args = parser.parse_args()

    app = args.app.resolve(strict=True)
    checkout = args.checkout.resolve(strict=True)
    out_dir = (args.out_dir or app.parent).resolve(strict=True)
    contents = app / 'Contents'
    executable = contents / 'MacOS' / 'Chromium'
    framework = contents / 'Frameworks' / 'Chromium Framework.framework' / 'Versions' / 'Current' / 'Chromium Framework'
    if not executable.is_file() or not framework.is_file():
        raise SystemExit('Chromium.app layout is incomplete: ' + str(app))
    if not (out_dir / 'args.gn').is_file():
        raise SystemExit('GN args are missing: ' + str(out_dir / 'args.gn'))

    script_root = Path(__file__).resolve().parent
    community = script_root.parent / 'editions/community/native'
    if not (community / 'community-v8.patch').is_file():
        community = script_root
    media = json.loads(subprocess.check_output([sys.executable, '-B', str(community / 'media_build.py'),
        '--verify', str(out_dir / 'args.gn')], text=True))
    patch_hashes = {
        'v8': digest(community / 'community-v8.patch'),
        'chromium': digest(community / 'community-chromium.patch'),
    }

    chromium_revision = git_revision(checkout)
    v8_revision = git_revision(checkout, 'v8')
    executable_hash = digest(executable)
    framework_hash = digest(framework)
    binaries = [
        dict(path='Chromium', bytes=executable.stat().st_size, sha256=executable_hash),
        dict(path='../Frameworks/Chromium Framework.framework/Versions/Current/Chromium Framework',
             bytes=framework.stat().st_size, sha256=framework_hash),
    ]
    command = [str(checkout / 'third_party/ninja/ninja'), '-C', out_dir.name, 'chrome', '-j', str(args.jobs)]
    attempt = 'macos-community-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    if args.validated_report:
        if not args.state.is_file():
            raise SystemExit('A matching compiled build state is required before marking validation')
        state = json.loads(args.state.read_text(encoding='utf-8-sig'))
        attempt = state.get('attemptId', attempt)
        if state.get('status') != 'compiled-not-validated' or state.get('exitCode') != 0:
            raise SystemExit('Existing build state is not a successful compile receipt')
        recorded = {row['path']: row['sha256'] for row in state.get('binaries', [])}
        if recorded != {row['path']: row['sha256'] for row in binaries}:
            raise SystemExit('Existing build state binary hashes differ from the app bundle')
        if state.get('patchHashes') != patch_hashes:
            raise SystemExit('Existing build state patch hashes differ from the current source projection')
        if state.get('releaseDefaults', {}).get('edition') != 'community':
            raise SystemExit('Existing build state edition differs from Community')
        chromium_revision = state.get('releaseDefaults', {}).get('chromiumRevision', chromium_revision)
        v8_revision = state.get('releaseDefaults', {}).get('v8Revision', v8_revision)
    else:
        state = dict(protocol='v8trace/build-supervisor/1', attemptId=attempt, status='compiled-not-validated',
            stage='awaiting-browser-validation', edition='community', checkout=str(checkout), output=str(out_dir),
            command=command, exitCode=0, binaries=binaries, patchHashes=patch_hashes,
            releaseDefaults=dict(edition='community', chromiumRevision=chromium_revision, v8Revision=v8_revision,
                gnArgsSha256=digest(out_dir / 'args.gn'), fieldTrialTestingConfig=False, mediaBuild=media,
                targetCpu='arm64', isDebug=False, isComponentBuild=False, platform='macos-arm64'))
        write_json(args.state, state)
    receipt_hash = digest(args.state)

    if not contains(framework, 'v8trace/browser-edition/1'):
        raise SystemExit('Community producer identity is missing from the framework')
    for token in ('V8TraceOpcode', 'V8TraceWasm', 'V8TraceCausality', 'V8TraceCausalEvent', '"vm-state"'):
        if contains(framework, token) or contains(executable, token):
            raise SystemExit('Professional producer remains in Community binary: ' + token)

    descriptor = dict(schemaVersion=1, backend='chromium-opcode', protocol='v8trace/opcode/1',
        category='disabled-by-default-v8.opcode', edition='community', editionProtocol='v8trace/edition/1',
        producerEditionProtocol='v8trace/browser-edition/1', chromiumRevision=chromium_revision,
        v8Revision=v8_revision, patchSha256=patch_hashes['v8'], sourcePatchHashes=patch_hashes,
        executableSha256=executable_hash,
        binaryHashes={'MacOS/Chromium': executable_hash,
            'Frameworks/Chromium Framework.framework/Versions/Current/Chromium Framework': framework_hash},
        builtAt=datetime.datetime.now(datetime.timezone.utc).isoformat(), opcodeSupported=False,
        professionalProducersIncluded=False, nativeRunValidated=False,
        buildEvidenceProtocol='v8trace/build-supervisor/1', buildAttemptId=attempt,
        buildReceiptSha256=receipt_hash, mediaBuild=media, fieldTrialTestingConfig=False,
        gnArgsSha256=digest(out_dir / 'args.gn'), apiKeyInfoBarSuppressed=True,
        startupProtocol='v8trace/lpac-startup/1', startupPatchSha256=patch_hashes['chromium'],
        startupDefaultsProtocol='v8trace/startup-defaults/1',
        imeCompatProtocol='v8trace/ime-compat/1', imeCompatPatchSha256=patch_hashes['chromium'],
        imeCompatScope='native-calls;thread-policy-restored;process-policy-preserved',
        languageProtocol='v8trace/languages/1', languagePatchSha256=patch_hashes['chromium'],
        preferredLanguages=['en-US', 'en'], applicationLocale='en-US',
        detailProtocol='v8trace/detail/1', detailFeatures=['jscall', 'native'],
        objectSnapshotProtocol='v8trace/objects/1', objectSwitchPatchSha256=patch_hashes['chromium'],
        detailFilterProtocol='v8trace/detail-filter/1', detailFilterPatchSha256=patch_hashes['v8'],
        detailHotPathProtocol='v8trace/detail-hotpath/1', callProtocol='v8trace/call/2',
        functionDictionaryProtocol='v8trace/function/1', callStatsProtocol='v8trace/call-stats/1',
        callValueFilterProtocol='v8trace/detail-filter/2', envProbeProtocol='v8trace/env/1',
        envStatsProtocol='v8trace/env-stats/1', envBuiltinProtocol='v8trace/env-builtin/1',
        envProbeFeatures=['absent', 'interfaces', 'mutations', 'inventory'],
        envProbePatchSha256=patch_hashes['v8'], closureProtocol='v8trace/closure/1',
        closurePatchSha256=patch_hashes['v8'], stateProtocol='v8trace/state/1',
        stateStatsProtocol='v8trace/state-stats/1', statePatchSha256=patch_hashes['chromium'],
        stateFeatures=['websocket', 'indexeddb', 'cookie', 'storage', 'wasm-module'],
        identityProtocol='v8trace/identity/1', identityPatchSha256=patch_hashes['v8'],
        auxProtocol='v8trace/aux/1', auxPatchSha256=patch_hashes['v8'],
        browserProtocol='v8trace/browser/1', browserFeatures=['env', 'http', 'script', 'console', 'exception'],
        browserPatchSha256=patch_hashes['chromium'], httpDrainProtocol='v8trace/http-drain/1',
        httpWireHeadersProtocol='v8trace/http-wire-headers/1',
        redirectBodyProtocol='v8trace/redirect-body/1',
        recordingControlProtocol='v8trace/recording/1',
        recordingControlPatchSha256=patch_hashes['chromium'],
        recordingControlTransport='local-files-no-cdp', traceSharedMemoryMaxBytes=8388608)

    if args.validated_report:
        report = json.loads(args.validated_report.read_text(encoding='utf-8-sig'))
        if report.get('conclusion') != 'PASS' or report.get('edition') != 'community':
            raise SystemExit('Validation report is not a matching Community PASS')
        if report.get('chromeSha256') != executable_hash:
            raise SystemExit('Validation report executable hash differs')
        if report.get('buildStateSha256') != receipt_hash:
            raise SystemExit('Validation report build state hash differs')
        descriptor['nativeRunValidated'] = True
        descriptor['validationEvidenceSha256'] = digest(args.validated_report)

    resources = contents / 'Resources'
    resources.mkdir(exist_ok=True)
    descriptor_file = resources / 'v8trace-opcode.json'
    write_json(descriptor_file, descriptor)
    inventory = dict(schemaVersion=1, protocol='v8trace/runtime-files/1', source='macOS app bundle',
        descriptorSha256=digest(descriptor_file), files=['MacOS/Chromium',
        'Frameworks/Chromium Framework.framework/Versions/Current/Chromium Framework',
        'Resources/v8trace-opcode.json', 'Resources/v8trace-runtime-files.json'])
    write_json(resources / 'v8trace-runtime-files.json', inventory)
    args.evidence.mkdir(parents=True, exist_ok=True)
    write_json(args.evidence / 'browser-package.json', dict(protocol='v8trace/macos-browser-package/1',
        status='validated' if args.validated_report else 'compiled-not-validated', edition='community', app=str(app), executable=str(executable),
        executableSha256=executable_hash, frameworkSha256=framework_hash,
        descriptorSha256=digest(descriptor_file), buildStateSha256=receipt_hash, command=command))
    print(json.dumps(dict(status='validated' if args.validated_report else 'compiled-not-validated', edition='community',
        executableSha256=executable_hash, frameworkSha256=framework_hash,
        descriptor=str(descriptor_file), descriptorSha256=digest(descriptor_file),
        buildState=str(args.state), buildStateSha256=receipt_hash), ensure_ascii=True))


if __name__ == '__main__':
    main()
