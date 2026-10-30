import argparse
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import threading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--edition', choices=('community', 'professional'), required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=False)
    checks = 0

    def check(value, message):
        nonlocal checks
        checks += 1
        if not value:
            raise AssertionError(message)

    flags = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0
    version = json.loads(subprocess.check_output([str(args.runtime), '--version'], creationflags=flags))
    check(version['edition']['id'] == args.edition, 'Actual CLI compiled identity')
    check(('社区版' if args.edition == 'community' else '专业版') in version['edition']['label'], 'Exact UTF-8 identity')
    incoming = queue.Queue()
    wire = (args.evidence / 'worker.stdout.jsonl').open('xb')
    errors = (args.evidence / 'worker.stderr.log').open('xb')
    process = subprocess.Popen([str(args.runtime), '--worker', '--assets', str(args.assets.resolve())], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, creationflags=flags)

    def reader():
        for line in process.stdout:
            wire.write(line)
            wire.flush()
            incoming.put(line)

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    def receive():
        return json.loads(incoming.get(timeout=30).decode('utf-8'))

    sequence = 0

    def request(command, **values):
        nonlocal sequence
        sequence += 1
        identity = str(sequence)
        process.stdin.write((json.dumps(dict(values, command=command, id=identity), ensure_ascii=False) + '\n').encode('utf-8'))
        process.stdin.flush()
        response = receive()
        check(response.get('id') == identity, 'Request/response identity')
        return response

    try:
        ready = receive()
        check(ready['type'] == 'ready' and ready['edition'] == version['edition'], 'Worker and CLI agree')
        check(len(ready['presets']) == 21, 'Complete catalog')
        check(sum(not item['available'] for item in ready['presets']) == (4 if args.edition == 'community' else 0), 'Exact restricted presets')
        check(len(ready['casePromptTemplates']) == (4 if args.edition == 'community' else 6), 'Edition case templates')
        base = dict(url='https://fixture.test/', output=str(args.evidence / 'must-not-create'), detailScriptInclude='fixture.js')
        for preset in ('env-protocol', 'env-target-detail'):
            selected = dict(base, preset=preset)
            if preset == 'env-protocol':
                selected.pop('detailScriptInclude')
            preview = request('config-preview', current={}, incoming=selected, mode='replace')
            check(preview['structuralPreflight']['ok'], 'Full environment configuration accepted')
            plan = preview['structuralPreflight']['plan']
            check(plan['edition'] == args.edition and plan['http'] and plan['redirectBodies'] and plan['scripts'], 'Full environment/HTTP contract')
            if preset == 'env-target-detail':
                check(plan['jsCallDetail'] and plan['nativeDetail'] and plan['jsCallValues'], 'Complete targeted call values')
        if args.edition == 'community':
            for option in ('opcode', 'wasm', 'vmpProbe', 'httpRootTrace', 'domTrace', 'cryptoTrace'):
                response = request('start', options=dict(base, http=True, **{option: True}))
                check(response['type'] == 'error' and option in response['error'] and 'Professional' in response['error'], 'Actual Worker rejects ' + option)
            response = request('causality-index', capture=str(args.evidence / 'not-a-capture'))
            check(response['type'] == 'studio-error' and 'Professional' in response['error'], 'Advanced command rejected before reading data')
        else:
            for preset in ('jsvmp-opcode', 'wasm-execution', 'http-root-chain'):
                selected = dict(base, preset=preset)
                selected.pop('detailScriptInclude')
                response = request('config-preview', current={}, incoming=selected, mode='replace')
                check(response['structuralPreflight']['ok'], 'Full Professional preset retained ' + preset)
        response = request('config-preview', current={}, incoming=dict(base, http=True, edition='community' if args.edition == 'professional' else 'professional'), mode='replace')
        check(response['type'] == 'studio-error' and 'build time' in response['error'], 'Runtime config does not upgrade or downgrade edition')
        response = request('case-prompt', templateId='env-reproduction', fields=dict(goal='中文😀完整证据', caseDirectory='case/中文案例'))
        check(response['type'] == 'case-prompt' and '中文😀完整证据' in response['text'] and 'case/中文案例' in response['text'], 'UTF-8 Chinese handoff')
        check(not (args.evidence / 'must-not-create').exists(), 'No browser/capture launch during preflight')
        response = request('shutdown')
        check(response['type'] == 'stopping' and not response['active'], 'Clean idle shutdown')
        process.stdin.close()
        check(process.wait(timeout=15) == 0, 'Worker exit status')
        thread.join(timeout=5)
        check(not thread.is_alive(), 'Reader drained')
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        process.stdout.close()
        thread.join(timeout=5)
        wire.close()
        errors.close()
    check((args.evidence / 'worker.stderr.log').stat().st_size == 0, 'No worker stderr output')
    result = dict(protocol='v8trace/edition-protocol-check/1', edition=args.edition, status='passed', checks=checks, runtimeSha256=hashlib.sha256(args.runtime.read_bytes()).hexdigest(), browserLaunched=False)
    (args.evidence / 'verification.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
