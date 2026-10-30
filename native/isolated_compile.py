import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import uuid


TARGET = 'obj/third_party/blink/renderer/core/core_hot/document.obj'
SOURCE = '../../third_party/blink/renderer/core/dom/document.cc'
COMPILER = '..\\..\\third_party\\llvm-build\\Release+Asserts\\bin\\clang-cl.exe'


def extract_edge(log):
    if 'LLVM ERROR: out of memory' not in log or 'Allocation failed' not in log:
        raise ValueError('A confirmed LLVM allocation failure is required')
    lines = log.splitlines()
    failed = [index for index, line in enumerate(lines) if line.startswith('FAILED: ')]
    if len(failed) != 1:
        raise ValueError('One ready-to-compile CXX edge is required')
    target = lines[failed[0]].removeprefix('FAILED: ').strip()
    if not target.startswith('obj/') or not target.endswith('.obj') or '..' in PurePosixPath(target).parts:
        raise ValueError('A scoped CXX object is required')
    command = lines[failed[0] + 1]
    prefix = COMPILER + ' /c '
    if not command.startswith(prefix) or ' /Fo' + target + ' ' not in command:
        raise ValueError('The pinned compiler command differs')
    source = command[len(prefix):].split(' /Fo', 1)[0]
    if (not source.startswith('../../') or '..' in PurePosixPath(source).parts[2:]
            or PurePosixPath(source).suffix not in ['.cc', '.cpp', '.c']
            or '/showIncludes:user' not in command or '\r' in command or '\n' in command
            or any(character in target + source for character in [' ', ':', '$', '"', '|', '&'])
            or any(part.startswith('@') for part in command.split())):
        raise ValueError('The pinned ready CXX paths or command differ')
    return dict(target=target, source=source, command=command)


def extract(log):
    edge = extract_edge(log)
    if edge['target'] != TARGET or edge['source'] != SOURCE:
        raise ValueError('Only the known ready-to-compile Document edge is supported')
    return edge['command']


def graph(command, target=TARGET, source=SOURCE):
    if any(character in command for character in ['\r', '\n']):
        raise ValueError('A single compiler command is required')
    for relative in [target, source]:
        path = PurePosixPath(relative)
        if path.is_absolute() or ':' in relative or '$' in relative or ' ' in relative:
            raise ValueError('Unexpected isolated edge path')
    if '..' in PurePosixPath(target).parts:
        raise ValueError('Isolated output leaves the build directory')
    return ('ninja_required_version = 1.7\nrule cxx\n  command = ' + command.replace('$', '$$')
        + '\n  description = CXX ${out}\n  deps = msvc\nbuild ' + target + ': cxx ' + source + '\n').encode('utf-8')


def command_sha256(command):
    return hashlib.sha256(command.encode('utf-8')).hexdigest()


def digest(file, limit=None):
    value = hashlib.sha256()
    with file.open('rb') as stream:
        while limit is None or limit:
            chunk = stream.read(min(limit, 1048576) if limit is not None else 1048576)
            if not chunk:
                break
            value.update(chunk)
            if limit is not None:
                limit -= len(chunk)
    return value.hexdigest()


def log_entries(file):
    result = {}
    for line in file.read_text('utf-8').splitlines():
        values = line.split('\t')
        if len(values) == 5:
            result[values[3]] = values[2:]
    return result


def run_build(supervisor, checkout, output, evidence, command, inputs, required_outputs, build_defaults=None):
    checkout, output = Path(checkout).resolve(), Path(output).resolve()
    evidence = Path(evidence)
    command = list(command)
    inputs = list(inputs)
    defaults = dict(build_defaults or {})
    recovered = []
    while True:
        try:
            return supervisor.run_build(checkout, output, evidence, command, inputs, required_outputs, build_defaults=defaults)
        except RuntimeError as original_error:
            receipt_file = evidence / 'build-state.json'
            if not receipt_file.is_file():
                raise
            previous = json.loads(receipt_file.read_text('utf-8'))
            try:
                edge = extract_edge(Path(previous['log']).read_text('utf-8'))
            except ValueError:
                raise original_error
            if len(recovered) >= 3 or edge['target'] in [row['target'] for row in recovered]:
                raise RuntimeError('Isolated CXX recovery boundary reached; original evidence preserved')
            if previous['status'] != 'failed' or previous.get('exitCode') != 1:
                raise RuntimeError('An exited failed Ninja is required for isolated recovery')
            if supervisor.active_ninja(output) or supervisor.fingerprint(checkout, inputs) != previous['sources']:
                raise RuntimeError('Native ownership or original build inputs changed')
            if (not (output / edge['source']).resolve().is_relative_to(checkout)
                    or not (output / edge['target']).resolve().is_relative_to(output)):
                raise RuntimeError('Isolated compiler paths leave the native workspace')
            folder = evidence / ('isolated-cxx-' + uuid.uuid4().hex)
            folder.mkdir(parents=True)
            shutil.copyfile(receipt_file, folder / 'parent-failed-build-state.json')
            graph_file = output / ('v8trace-isolated-ready-' + uuid.uuid4().hex + '.ninja')
            graph_file.write_bytes(graph(edge['command'], edge['target'], edge['source']))
            inputs.append(graph_file.relative_to(checkout).as_posix())
            before = log_entries(output / '.ninja_log')
            deps = output / '.ninja_deps'
            deps_size, deps_hash = deps.stat().st_size, digest(deps)
            for name in ['.ninja_log', '.ninja_deps']:
                shutil.copyfile(output / name, folder / (name + '.before'))
            mini = supervisor.run_build(checkout, output, folder / 'compile',
                [command[0], '-C', str(output), '-f', graph_file.name, edge['target'], '-j', '1'],
                inputs, [edge['target']], build_defaults=dict(defaults, isolatedReadyTranslationUnit=True, compilerFlagsUnchanged=True))
            after = log_entries(output / '.ninja_log')
            if any(after.get(name) != value for name, value in before.items() if name != edge['target']):
                raise RuntimeError('Unrelated Ninja command records changed during isolated recovery')
            if deps.stat().st_size < deps_size or digest(deps, deps_size) != deps_hash:
                raise RuntimeError('Dependency journal compacted during isolated recovery; cache backups preserved')
            row = dict(target=edge['target'], objectSha256=mini['binaries'][0]['sha256'],
                compilerCommandSha256=command_sha256(edge['command']), compilerFlagsUnchanged=True,
                graphSha256=digest(graph_file), isolatedReceiptSha256=digest(folder / 'compile/build-state.json'))
            recovered.append(row)
            defaults['isolatedTranslationUnits'] = list(recovered)
            if '-j' in command:
                index = command.index('-j') + 1
                if int(command[index]) > 1:
                    command[index] = '1'
                    defaults.update(ninjaJobs=1, parallelismReducedAfterAllocationFailure=True)
            supervisor.atomic_json(folder / 'recovery.json', dict(conclusion='PASS', **row,
                unrelatedCommandEntriesRetained=True, dependencyJournalAppendOnly=True))
