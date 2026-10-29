import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--assets', required=True)
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix='v8trace-file-logging-')).resolve()
    checks = 0
    try:
        command = [str(Path(args.runtime).resolve())]
        version = subprocess.run(command + ['--version'], cwd=root, capture_output=True, timeout=30)
        require(version.returncode == 0 and json.loads(version.stdout)['runtime'] == 'cpp',
                'Machine version protocol remains available')
        checks += 1
        config = root / '配置.json'
        config.write_text(json.dumps(dict(output='logs/中文-😀', url='https://fixture.test/',
            chromePath=str(root / 'missing-chrome.exe'), transport='native', http=True),
            ensure_ascii=False), encoding='utf-8')
        for action in ('capture', 'import'):
            arguments = command + [action, '--config', str(config), '--assets', args.assets]
            if action == 'import':
                arguments += ['--trace', str(root / 'missing-trace.json')]
            result = subprocess.run(arguments, cwd=root, capture_output=True, timeout=30)
            require(result.returncode == 1 and not result.stdout and not result.stderr,
                    action + ' failure is file-only, including stderr')
            checks += 1
        target = root / 'logs/中文-😀.runtime.jsonl'
        records = [json.loads(line) for line in target.read_text('utf-8').splitlines()]
        require(len(records) == 2 and all(row['protocol'] == 'v8trace/runtime-log/1'
                and row['record']['type'] == 'error' and row['observedAt'] for row in records),
                'UTF-8 file contains both complete structured error records')
        checks += 1
        require(not (root / 'logs/中文-😀').exists(), 'Preflight error does not create a false capture directory')
        checks += 1
        trace = root / 'trace.json'
        trace.write_text(json.dumps(dict(traceEvents=[])), encoding='utf-8')
        imported = subprocess.run(command + ['import', '--config', str(config), '--assets', args.assets,
            '--trace', str(trace), '--output', 'import-ok'], cwd=root, capture_output=True, timeout=30)
        require(imported.returncode == 0 and not imported.stdout and not imported.stderr,
                'Successful import also produces no console output')
        checks += 1
        summary = json.loads((root / 'import-ok/summary.json').read_text('utf-8'))
        terminal = json.loads((root / 'import-ok.runtime.jsonl').read_text('utf-8').splitlines()[-1])
        require(terminal['record'] == summary and summary['captureConclusion'] == 'imported',
                'Complete final summary is directly persisted in runtime log')
        checks += 1
        override = subprocess.run(command + ['capture', '--config', str(config), '--assets', args.assets,
            '--output', 'other/override'], cwd=root, capture_output=True, timeout=30)
        require(override.returncode == 1 and not override.stdout and not override.stderr
                and (root / 'other/override.runtime.jsonl').is_file(), 'Output override also relocates runtime log')
        checks += 1
        invalid = root / 'invalid.json'
        invalid.write_text('{', encoding='utf-8')
        result = subprocess.run(command + ['capture', '--config', str(invalid)], cwd=root,
                                capture_output=True, timeout=30)
        require(result.returncode == 1 and not result.stdout and not result.stderr
                and (root / 'v8trace-runtime.jsonl').is_file(), 'Unreadable configuration logs in current directory')
        checks += 1
        (root / 'blocked').write_bytes(b'owned-test-file')
        result = subprocess.run(command + ['capture', '--config', str(config), '--assets', args.assets,
            '--output', 'blocked/capture'], cwd=root, capture_output=True, timeout=30)
        require(result.returncode == 74 and not result.stdout and not result.stderr,
                'File logging failure uses nonzero status without console fallback')
        checks += 1
        print(json.dumps(dict(conclusion='PASS', checks=checks, captureConsoleOutput=False,
                              utf8Files=True, machineProtocolsPreserved=True)))
    finally:
        if root.parent != Path(tempfile.gettempdir()).resolve() or not root.name.startswith('v8trace-file-logging-'):
            raise RuntimeError('Owned file-logging fixture cleanup boundary changed')
        shutil.rmtree(root)


if __name__ == '__main__':
    main()
