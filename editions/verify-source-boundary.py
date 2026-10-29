import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def sha256(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--edition', choices=('community', 'professional'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    commands_file = args.build.resolve() / 'compile_commands.json'
    commands = json.loads(commands_file.read_text('utf-8-sig'))
    captured_commands = None
    if not any('v8trace_core.dir' in entry['command'] for entry in commands):
        cache = (args.build / 'CMakeCache.txt').read_text('utf-8-sig')
        ninja = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$', cache, re.MULTILINE)
        if not ninja:
            raise RuntimeError('Configured Ninja executable not found')
        rules_text = (args.build / 'CMakeFiles/rules.ninja').read_text('utf-8-sig')
        rules = re.findall(r'^rule (CXX_COMPILER__v8trace_core_\w+)$', rules_text, re.MULTILINE)
        if not rules:
            raise RuntimeError('Actual generated core compiler rules not found')
        result = subprocess.run([ninja.group(1).strip(), '-t', 'compdb', *rules], cwd=args.build, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        commands = json.loads(result.stdout)
        commands_file = args.output.with_suffix('.commands.json')
        captured_commands = result.stdout
    core = set()
    for entry in commands:
        if 'v8trace_core.dir' not in entry['command']:
            continue
        file = Path(entry['file'])
        if not file.is_absolute():
            file = Path(entry['directory']) / file
        file = file.resolve(strict=True)
        if not file.is_relative_to(root):
            raise RuntimeError('Core source leaves project: ' + str(file))
        core.add(file.relative_to(root).as_posix())
    if not core or 'runtime-cpp/src/edition.cpp' not in core:
        raise RuntimeError('Actual core compile commands not found')
    professional = {file for file in core if file.startswith('professional/')}
    expected = {
        'professional/runtime/src/causality.cpp',
        'professional/runtime/src/causal_queries.cpp',
        'professional/runtime/src/jsvmp.cpp',
        'professional/runtime/src/vmp_probe.cpp',
    }
    community_stub = 'runtime-cpp/src/community_services.cpp'
    if args.edition == 'community':
        if professional or community_stub not in core:
            raise RuntimeError('Community build includes Professional implementations or lacks explicit boundary stubs')
    elif professional != expected or community_stub in core:
        raise RuntimeError('Professional build lost an implementation or contains Community boundary stubs')
    if captured_commands is not None:
        commands_file.parent.mkdir(parents=True, exist_ok=True)
        with commands_file.open('xb') as file:
            file.write(captured_commands)
    report = {
        'protocol': 'v8trace/edition-source-boundary/1',
        'edition': args.edition,
        'status': 'runtime-compile-source-boundary-passed',
        'commandsSha256': sha256(commands_file),
        'coreSources': [{'path': file, 'sha256': sha256(root / file)} for file in sorted(core)],
        'professionalImplementationCount': len(professional),
        'nativeSourceExportValidated': False,
        'publicSourceReleaseEligible': False,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x', encoding='utf-8', newline='\n') as file:
        json.dump(report, file, ensure_ascii=False, indent=2)
        file.write('\n')
    print(json.dumps({key: value for key, value in report.items() if key != 'coreSources'}))


if __name__ == '__main__':
    main()
