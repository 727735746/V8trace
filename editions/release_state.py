import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid


PHASES = ('build', 'observer', 'validation', 'package', 'publication')


def file_sha256(file):
    result = hashlib.sha256()
    with Path(file).open('rb') as stream:
        for data in iter(lambda: stream.read(1048576), b''):
            result.update(data)
    return result.hexdigest()


def process_identity(pid):
    if isinstance(pid, bool) or not isinstance(pid, int) or pid < 1:
        raise ValueError('A positive PID is required')
    if os.name == 'nt':
        command = ("[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"
                   "$r=Get-CimInstance Win32_Process -Filter 'ProcessId=" + str(pid) + "' -ErrorAction Stop;"
                   "if($null -ne $r){[ordered]@{pid=[int]$r.ProcessId;"
                   "creationToken=$r.CreationDate.ToUniversalTime().ToString('o');"
                   "commandLine=[string]$r.CommandLine;executable=[string]$r.ExecutablePath}|ConvertTo-Json -Compress}")
        result = subprocess.run(['pwsh', '-NoProfile', '-NonInteractive', '-Command', command],
                                capture_output=True, encoding='utf-8', check=True, timeout=30,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        value = json.loads(result.stdout) if result.stdout.strip() else None
    elif sys.platform.startswith('linux'):
        try:
            root = Path('/proc') / str(pid)
            fields = (root / 'stat').read_text().rsplit(')', 1)[1].split()
            if fields[0] == 'Z':
                return None
            argv = [os.fsdecode(item) for item in (root / 'cmdline').read_bytes().split(b'\0') if item]
            value = dict(pid=pid, creationToken=Path('/proc/sys/kernel/random/boot_id').read_text().strip() + ':' + fields[19],
                         commandLine=json.dumps(argv), executable=os.readlink(root / 'exe'))
        except (FileNotFoundError, ProcessLookupError):
            return None
    else:
        raise RuntimeError('Process birth identity is not implemented on this platform')
    if value is not None and (value.get('pid') != pid or any(not value.get(key) for key in ('creationToken', 'commandLine', 'executable'))):
        raise RuntimeError('Process identity is incomplete; retain the existing owner')
    return value


def validate_resume(receipt, checkout, output, command, fingerprint, *, output_locked, query=None):
    if not output_locked:
        raise RuntimeError('Resume requires the exclusive build output lease')
    checkout, output = Path(checkout).resolve(), Path(output).resolve()
    if (receipt.get('protocol') != 'v8trace/build-supervisor/1'
            or Path(receipt.get('checkout', '')).resolve() != checkout
            or Path(receipt.get('output', '')).resolve() != output
            or output == checkout or not output.is_relative_to(checkout)
            or receipt.get('command') != command or not isinstance(command, list) or not command):
        raise ValueError('Resume checkout/output/command differs from its receipt')
    sources = receipt.get('sources')
    if not isinstance(sources, list) or not sources or fingerprint(checkout, [row['path'] for row in sources]) != sources:
        raise RuntimeError('Resume input fingerprints differ')
    query = query or process_identity
    for pid_key, identity_key in (('supervisorPid', 'ownerIdentity'), ('ninjaPid', 'processIdentity')):
        pid = receipt.get(pid_key)
        if pid is None:
            continue
        current = query(pid)
        if current is None:
            continue
        expected = receipt.get(identity_key)
        if not expected or any(current.get(key) != expected.get(key) for key in ('pid', 'creationToken', 'commandLine', 'executable')):
            raise RuntimeError('Live PID identity differs or was not recorded: ' + str(pid))
        raise RuntimeError('An unchanged live owner still holds this build; do not start another Ninja')
    return dict(protocol='v8trace/resume-check/1', inputIdentityVerified=True, commandIdentityVerified=True,
                outputLeaseHeld=True, liveOwnerObserved=False, startsProcess=False)


def phase_receipt(evidence, phase, status, bindings=None, parents=()):
    states = {
        'build': ('pending', 'running', 'compiled-not-validated', 'failed', 'interrupted'),
        'observer': ('pending', 'running', 'completed', 'failed', 'interrupted'),
        'validation': ('pending', 'running', 'passed', 'failed'),
        'package': ('pending', 'running', 'assembled-unverified', 'validated', 'failed'),
        'publication': ('not-requested', 'manual-receipt'),
    }
    if phase not in states or status not in states[phase]:
        raise ValueError('Invalid release phase/status')
    inputs = []
    for name, raw in sorted((bindings or {}).items()):
        file = Path(raw).resolve(strict=True)
        inputs.append(dict(name=name, path=str(file), bytes=file.stat().st_size, sha256=file_sha256(file)))
    references = []
    for raw in parents:
        file = Path(raw).resolve(strict=True)
        raw = file.read_bytes()
        data = json.loads(raw)
        if data.get('protocol') != 'v8trace/release-phase/1' or data.get('phase') not in PHASES:
            raise ValueError('Invalid parent phase receipt')
        references.append(dict(path=str(file), phase=data['phase'], status=data['status'], sha256=hashlib.sha256(raw).hexdigest()))
    record = dict(protocol='v8trace/release-phase/1', phase=phase, status=status, attemptId=uuid.uuid4().hex,
                  recordedAt=datetime.datetime.now(datetime.timezone.utc).isoformat(), bindings=inputs, parents=references,
                  remoteActionPerformed=False, observationOnly=True)
    evidence = Path(evidence).resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    file = evidence / (phase + '-' + record['attemptId'] + '.json')
    temporary = file.with_suffix('.tmp')
    with temporary.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(record, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.link(temporary, file)
    try:
        temporary.unlink()
    except OSError as error:
        print(json.dumps(dict(receipt=str(file), committed=True, temporary=str(temporary), cleanupError=repr(error))), file=sys.stderr)
    return file


def main():
    parser = argparse.ArgumentParser(description='Append a local phase observation; does not build, validate, package or publish.')
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--phase', choices=PHASES, required=True)
    parser.add_argument('--status', required=True)
    parser.add_argument('--parent', type=Path, action='append', default=[])
    parser.add_argument('--bind', action='append', default=[], metavar='NAME=FILE')
    args = parser.parse_args()
    bindings = {}
    for value in args.bind:
        name, file = value.split('=', 1)
        if not name or name in bindings:
            raise ValueError('Unique binding names are required')
        bindings[name] = file
    file = phase_receipt(args.evidence, args.phase, args.status, bindings, args.parent)
    print(json.dumps(dict(receipt=str(file), sha256=file_sha256(file), remoteActionPerformed=False)))


if __name__ == '__main__':
    main()
