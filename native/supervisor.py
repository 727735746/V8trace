import contextlib
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import time
import uuid


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def atomic_json(path, value):
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        with temporary.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        for attempt in range(10):
            try:
                os.replace(temporary, path)
                break
            except PermissionError as error:
                if getattr(error, 'winerror', None) not in (5, 32, 33) or attempt == 9:
                    raise
                time.sleep(.05 * (attempt + 1))
    finally:
        temporary.unlink(missing_ok=True)


@contextlib.contextmanager
def build_lock(output):
    output.mkdir(parents=True, exist_ok=True)
    with (output / '.v8trace-build.lock').open('a+b') as stream:
        stream.seek(0)
        if os.name == 'nt':
            import msvcrt
            if stream.read(1) == b'':
                stream.write(b'0')
                stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            if os.name == 'nt':
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def file_sha256(file):
    digest = hashlib.sha256()
    with file.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1048576), b''):
            digest.update(chunk)
    return digest.hexdigest()


def fingerprint(checkout, relative_files):
    result = []
    for relative in sorted(set(relative_files)):
        file = (checkout / relative).resolve()
        if not file.is_relative_to(checkout):
            raise ValueError('Build input leaves checkout: ' + relative)
        result.append({'path': relative, 'sha256': file_sha256(file) if file.is_file() else None})
    return result


def active_ninja(output):
    if os.name != 'nt':
        return []
    command = "Get-CimInstance Win32_Process -Filter \"Name='ninja.exe'\" | Select-Object ProcessId,CommandLine | ConvertTo-Json -Compress"
    process = subprocess.run(['pwsh', '-NoProfile', '-Command', command], capture_output=True, text=True, encoding='utf-8', timeout=30)
    if process.returncode:
        raise RuntimeError('Ninja process inventory failed: ' + process.stderr)
    value = json.loads(process.stdout or '[]')
    rows = value if isinstance(value, list) else [value]
    variants = [output.as_posix().lower(), ('out/' + output.name).lower()]
    return [row for row in rows if any(item in str(row.get('CommandLine', '')).replace('\\', '/').lower() for item in variants)]

def release_tools():
    here = Path(__file__).resolve()
    for file in (here.parents[1] / 'editions/release_state.py', here.parents[2] / 'release_state.py'):
        if file.is_file():
            spec = importlib.util.spec_from_file_location('v8trace_release_state', file)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            return module
    raise RuntimeError('Paired release_state.py is required with this supervisor')

def run_build(checkout, output, evidence, command, inputs, required_outputs, heartbeat=10, build_defaults=None, resume_state=None):
    checkout, output, evidence = checkout.resolve(), output.resolve(), evidence.resolve()
    if not output.is_relative_to(checkout) or output == checkout:
        raise ValueError('Build output must be inside checkout')
    evidence.mkdir(parents=True, exist_ok=True)
    with build_lock(output):
        other = active_ninja(output)
        if other:
            raise RuntimeError('Existing Ninja owns the output: ' + json.dumps(other))
        tools = release_tools()
        if resume_state is not None:
            tools.validate_resume(resume_state, checkout, output, command, fingerprint, output_locked=True)
        status_file = evidence / 'build-state.json'
        previous = None
        if status_file.exists():
            try:
                previous = json.loads(status_file.read_text(encoding='utf-8'))
            except (ValueError, UnicodeError):
                previous = {'status': 'corrupt-state', 'rawSha256': hashlib.sha256(status_file.read_bytes()).hexdigest()}
            (evidence / ('previous-state-' + uuid.uuid4().hex + '.json')).write_bytes(status_file.read_bytes())
        attempt = uuid.uuid4().hex
        log = evidence / ('ninja-' + attempt + '.log')
        sources = fingerprint(checkout, inputs)
        if resume_state is not None and sources != resume_state['sources']:
            raise RuntimeError('Resume inputs changed before launch')
        state = {'protocol': 'v8trace/build-supervisor/1', 'attemptId': attempt, 'status': 'running',
                 'stage': 'ninja', 'startedAt': now(), 'supervisorPid': os.getpid(), 'checkout': str(checkout),
                 'output': str(output), 'command': command, 'log': str(log), 'sources': sources,
                 'previousStatus': previous.get('status') if isinstance(previous, dict) else None,
                 'browserValidated': False, 'releaseEligible': False,
                 'ownerIdentity': tools.process_identity(os.getpid()),
                 'phases': {'build': 'running', 'observer': 'running', 'validation': 'pending',
                            'package': 'pending', 'publication': 'not-requested'}}
        if build_defaults is not None:
            state['releaseDefaults'] = build_defaults
        def save():
            state['updatedAt'] = now()
            try:
                atomic_json(status_file, state)
            except OSError as error:
                state['phases']['observer'] = 'failed'
                state['observerError'] = repr(error)
                raise
        save()
        process = None
        try:
            with log.open('xb') as stream:
                creationflags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
                process = subprocess.Popen(command, cwd=checkout, stdout=stream, stderr=subprocess.STDOUT, creationflags=creationflags)
                state['ninjaPid'] = process.pid
                try:
                    state['processIdentity'] = tools.process_identity(process.pid)
                except BaseException as error:
                    state['phases']['observer'] = 'failed'
                    state['observerError'] = repr(error)
                    raise
                save()
                while process.poll() is None:
                    state['logBytes'] = log.stat().st_size
                    save()
                    time.sleep(heartbeat)
                state['exitCode'] = process.returncode
            if process.returncode:
                raise RuntimeError('Ninja exited with ' + str(process.returncode))
            if fingerprint(checkout, inputs) != sources:
                raise RuntimeError('Checkout inputs changed during build; no successful receipt')
            binaries = []
            for relative in required_outputs:
                file = (output / relative).resolve()
                if not file.is_relative_to(output) or not file.is_file():
                    raise RuntimeError('Required compiled output missing: ' + relative)
                binaries.append({'path': relative, 'bytes': file.stat().st_size, 'sha256': file_sha256(file)})
            state.update(status='compiled-not-validated', stage='awaiting-browser-validation', binaries=binaries)
            state['phases'].update(build='compiled-not-validated', observer='completed')
        except BaseException as error:
            if process is not None and process.poll() is None:
                state['phases']['observer'] = 'failed'
                state['observerError'] = repr(error)
                state['exitCode'] = process.wait()
            state['failureStage'] = 'observer' if state['phases']['observer'] == 'failed' else 'build'
            state['phases']['build'] = 'exited' if 'exitCode' in state else 'unknown'
            state.update(status='failed', error=repr(error))
            raise
        finally:
            state['finishedAt'] = now()
            save()
        return state


def release_defaults(output):
    data = (output / 'args.gn').read_bytes()
    assignments = re.findall(r'(?m)^[ \t]*disable_fieldtrial_testing_config[ \t]*=[ \t]*([^\r\n]*)', data.decode('utf-8-sig'))
    if [value.split('#', 1)[0].strip() for value in assignments] != ['true']:
        raise ValueError('Release Chromium requires disable_fieldtrial_testing_config = true; run Configure before Build/Package')
    return {'protocol': 'v8trace/startup-defaults/1', 'fieldTrialTestingConfig': False,
            'gnArgsSha256': hashlib.sha256(data).hexdigest()}


