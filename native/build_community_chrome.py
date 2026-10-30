import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tempfile
import uuid

def local_module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


metadata = local_module('browser_metadata')
supervisor = local_module('supervisor')
media_build = local_module('media_build')
isolated_compile = local_module('isolated_compile')


def run(command, cwd, evidence, name):
    stem = name + '-' + uuid.uuid4().hex
    output = evidence / (stem + '.stdout.log')
    with output.open('xb') as out, (evidence / (stem + '.stderr.log')).open('xb') as err:
        result = subprocess.run(command, cwd=cwd, stdout=out, stderr=err)
    if result.returncode:
        raise RuntimeError(name + ' failed: ' + str(result.returncode))
    return output


def source_paths(checkout, recipe):
    paths = []
    for row in recipe['files']:
        relative = PurePosixPath(row['path'])
        if relative.is_absolute() or '..' in relative.parts or ':' in str(relative):
            raise RuntimeError('Source path leaves checkout')
        file = checkout.joinpath(*relative.parts)
        if not file.resolve().is_relative_to(checkout):
            raise RuntimeError('Source path resolves outside checkout')
        paths.append((row, file))
    return paths


def source_hash(file):
    return metadata.digest(file) if file.is_file() else None


def limit_linker_threads(data):
    counts = dict(peLinkRules=0, rustLinkRules=0, librarianRules=0)
    marker = b'lld-link.exe '
    lines = []
    for line in data.splitlines(keepends=True):
        if marker not in line:
            lines.append(line)
            continue
        line = line.replace(marker + b'/threads:1 ', marker)
        line = line.replace(marker + b'-Clink-arg=/threads:1 ', marker)
        if b'/threads:' in line:
            raise RuntimeError('Unexpected pinned linker thread argument')
        tail = line.split(marker, 1)[1]
        if line.lstrip().startswith(b'command = ') and tail.startswith(b'/lib '):
            counts['librarianRules'] += 1
        elif line.lstrip().startswith(b'command = '):
            line = line.replace(marker, marker + b'/threads:1 ', 1)
            counts['peLinkRules'] += 1
        elif line.lstrip().startswith(b'rspfile_content = ') and b'-Clinker=' in line:
            line = line.replace(marker, marker + b'-Clink-arg=/threads:1 ', 1)
            counts['rustLinkRules'] += 1
        else:
            raise RuntimeError('Unknown pinned linker rule mode')
        lines.append(line)
    if not counts['peLinkRules']:
        raise RuntimeError('Expected pinned linker command missing')
    return b''.join(lines), counts


def prepare_compile_graph(checkout, output, evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    ninja = checkout / 'third_party/ninja/ninja.exe'
    run([str(ninja), '-C', str(output), 'build.ninja', '-j', '1'], checkout, evidence, 'sync-manifest')
    toolchain = output / 'toolchain.ninja'
    data, modes = limit_linker_threads(toolchain.read_bytes())
    toolchain.write_bytes(data)
    plan = run([str(ninja), '-C', str(output), '-n', 'chrome', '-j', '1'], checkout, evidence, 'compile-plan')
    with plan.open('rb') as stream:
        if any(b'Regenerating ninja files' in line for line in stream):
            raise RuntimeError('Ninja would regenerate frozen linker rules again')
    return dict(protocol='v8trace/native-compile-graph/1', linkerThreads=1, ruleModes=modes,
        generatedToolchainSha256=metadata.digest(toolchain), plan=str(plan), planSha256=metadata.digest(plan))


def verify(checkout, recipe):
    for row, file in source_paths(checkout, recipe):
        if source_hash(file) != row['sha256']:
            raise RuntimeError('Community source differs: ' + row['path'])


def apply(checkout, recipe, native, evidence):
    paths = source_paths(checkout, recipe)
    if all(source_hash(file) == row['sha256'] for row, file in paths):
        return
    for row, file in paths:
        if source_hash(file) not in [row['stockSha256'], row['sha256']]:
            raise RuntimeError('Unknown local source edit preserved: ' + row['path'])
    with tempfile.TemporaryDirectory(prefix='v8trace-community-patches-') as temporary:
        tree = Path(temporary)
        run(['git', 'init', '--quiet', str(tree)], tree, evidence, 'init-patch-tree')
        (tree / 'v8').mkdir()
        run(['git', 'init', '--quiet', str(tree / 'v8')], tree, evidence, 'init-v8-patch-tree')
        repositories = sorted(recipe['dependencyRevisions'], key=len, reverse=True)
        for row, file in paths:
            if row['stockSha256'] is None:
                continue
            if source_hash(file) == row['stockSha256']:
                data = file.read_bytes()
            else:
                owner = next(key for key in repositories if key == '.' or row['path'].startswith(key + '/'))
                root = checkout if owner == '.' else checkout / owner
                relative = row['path'] if owner == '.' else row['path'][len(owner) + 1:]
                data = subprocess.check_output(['git', '-C', str(root), 'cat-file', 'blob', 'HEAD:' + relative])
            if hashlib.sha256(data).hexdigest() != row['stockSha256']:
                raise RuntimeError('Pinned stock source differs: ' + row['path'])
            destination = tree / row['path']
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        for role, root in [('chromium', tree), ('v8', tree / 'v8')]:
            patch = native / ('community-' + role + '.patch')
            if metadata.digest(patch) != recipe['patchHashes'][role]:
                raise RuntimeError('Published patch hash differs')
            git = ['git', '-c', 'core.autocrlf=false', '-c', 'core.eol=lf', '-C', str(root), 'apply']
            run([*git, '--check', '--whitespace=nowarn', str(patch)], tree, evidence, 'check-' + role)
            run([*git, '--whitespace=nowarn', str(patch)], tree, evidence, 'apply-' + role)
        for row, file in paths:
            data = (tree / row['path']).read_bytes()
            if hashlib.sha256(data).hexdigest() != row['sha256']:
                raise RuntimeError('Projected source differs: ' + row['path'])
            if source_hash(file) == row['sha256']:
                continue
            if source_hash(file) != row['stockSha256']:
                raise RuntimeError('Source changed during preparation: ' + row['path'])
            file.parent.mkdir(parents=True, exist_ok=True)
            temporary_file = file.with_name(file.name + '.v8trace-' + uuid.uuid4().hex + '.tmp')
            try:
                temporary_file.write_bytes(data)
                os.replace(temporary_file, file)
            finally:
                temporary_file.unlink(missing_ok=True)
    verify(checkout, recipe)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--out-name', default='V8TraceCommunity')
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--stage', choices=['all', 'apply', 'verify', 'configure', 'prepare-graph', 'build', 'package'], default='all')
    parser.add_argument('--jobs', type=int, choices=range(1, 33), default=1)
    args = parser.parse_args()
    if os.name != 'nt':
        raise RuntimeError('This native build recipe targets Windows x64')
    native = Path(__file__).resolve().parent
    source_file = native / 'community-source.json'
    recipe = metadata.load(source_file)
    checkout = args.checkout.resolve(strict=True)
    if not args.out_name or Path(args.out_name).name != args.out_name or ':' in args.out_name:
        raise RuntimeError('A single output directory name is required')
    output = (checkout / 'out' / args.out_name).resolve()
    if output.parent != checkout / 'out':
        raise RuntimeError('Native output leaves checkout/out')
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    if (checkout / '.v8trace-community-source-window.json').exists():
        raise RuntimeError('An internal source window owns this checkout')
    for name, revision in recipe['dependencyRevisions'].items():
        root = checkout if name == '.' else checkout / name
        actual = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != revision:
            raise RuntimeError('Pinned checkout revision differs: ' + name)
    if args.stage == 'prepare-graph':
        media_build.verify(output / 'args.gn')
        print(json.dumps(prepare_compile_graph(checkout, output, evidence)))
        return
    if args.stage in ['all', 'apply']:
        apply(checkout, recipe, native, evidence)
    verify(checkout, recipe)
    gn = checkout / 'buildtools/win/gn.exe'
    if args.stage in ['all', 'configure']:
        output.mkdir(parents=True, exist_ok=True)
        (output / 'args.gn').write_text((native / 'args.gn').read_text('utf-8'), encoding='utf-8')
        run([str(gn), 'gen', str(output), '--root-target=//chrome', '--root-pattern=//chrome:chrome'], checkout, evidence, 'gn')
    if args.stage in ['all', 'build']:
        graph = prepare_compile_graph(checkout, output, evidence / 'graph')
        inputs = [row['path'] for row in recipe['files']]
        inputs += [(output / name).relative_to(checkout).as_posix() for name in ['args.gn', 'toolchain.ninja']]
        defaults = supervisor.release_defaults(output)
        defaults.update(edition='community', sourceProjectionSha256=metadata.digest(source_file), linkerThreads=1,
            generatedToolchainSha256=graph['generatedToolchainSha256'], mediaBuild=media_build.verify(output / 'args.gn'))
        isolated_compile.run_build(supervisor, checkout, output, evidence / 'compile',
            [str(checkout / 'third_party/ninja/ninja.exe'), '-C', str(output), 'chrome', '-j', str(args.jobs)],
            inputs, ['chrome.exe', 'chrome.dll', 'chrome_elf.dll', 'v8_context_snapshot.bin'], build_defaults=defaults)
    if args.stage in ['all', 'package']:
        dependencies = run([str(gn), 'desc', str(output), '//chrome:chrome', 'runtime_deps', '--format=json'], checkout, evidence, 'runtime-deps')
        result = metadata.write_metadata(output, source_file, evidence / 'compile/build-state.json', dependencies)
        supervisor.atomic_json(evidence / 'browser-package.json', result)
    print(json.dumps(dict(stage=args.stage, edition='community', output=str(output), nativeRunValidated=False)))


if __name__ == '__main__':
    main()
