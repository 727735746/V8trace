import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ninja', required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--edition', choices=('community', 'professional'), default='professional')
    args = parser.parse_args()
    root = Path(args.build).resolve()
    checks = [
        ('CMakeFiles/v8trace-causality-messages-tests.dir/tests/causality_messages_tests.cpp.obj',
         '/native/v8trace-causal-messages.h'),
        ('CMakeFiles/v8trace_core.dir/src/options.cpp.obj', '/include/v8trace/core.h'),
        ('CMakeFiles/v8trace-causality-continuations-tests.dir/tests/causality_continuations_tests.cpp.obj',
         '/native/v8trace-causal-continuations.h'),
    ]
    if args.edition == 'community':
        checks = [
            ('CMakeFiles/v8trace_core.dir/src/options.cpp.obj', '/include/v8trace/core.h'),
            ('CMakeFiles/v8trace-jscall-tests.dir/tests/jscall_tests.cpp.obj', '/native/v8trace-call-ledger.h'),
        ]
    checks.append(('CMakeFiles/v8trace_core.dir/src/edition.cpp.obj', '/generated/v8trace/edition_build.h'))
    rows = []
    for object_file, expected in checks:
        result = subprocess.run([args.ninja, '-C', str(root), '-t', 'deps', object_file],
                                capture_output=True, timeout=25)
        result.check_returncode()
        output = result.stdout.decode('utf-8', errors='replace')
        count = re.search(r'#deps (\d+)', output)
        dependencies = []
        for line in output.splitlines():
            if not line.startswith('    '):
                continue
            file = Path(line.strip())
            if not file.is_absolute():
                file = root / file
            dependencies.append(file.resolve().as_posix().lower())
        observed = any(file.endswith(expected) for file in dependencies)
        rows.append(dict(object=object_file, dependencyCount=int(count[1]) if count else 0,
                         expectedHeader=expected, headerObserved=observed))
    prefix_observed = 'msvc_deps_prefix = V8TRACE_INCLUDE: ' in (root / 'CMakeFiles/rules.ninja').read_text('utf-8')
    report = dict(status='PASS' if prefix_observed and all(row['dependencyCount'] > 0 and row['headerObserved']
                                     for row in rows) else 'FAIL', checks=rows, stableAsciiPrefix=prefix_observed)
    (root / 'compiler-dependency-check.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    if report['status'] != 'PASS':
        raise AssertionError('MSVC header dependencies are missing: ' + json.dumps(rows))
    print(json.dumps(report))


if __name__ == '__main__':
    main()
