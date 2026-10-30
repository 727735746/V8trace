import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import tempfile
import time


def digest(file):
    value = hashlib.sha256()
    with Path(file).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1048576), b''):
            value.update(chunk)
    return value.hexdigest()


def checked_path(file):
    file = Path(os.path.abspath(file))
    for part in (file, *file.parents):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError('Export paths must not traverse links: ' + str(part))
    return file


def validate_manifest(output):
    output = checked_path(output)
    file = output / 'SOURCE-MANIFEST.json'
    manifest = json.loads(file.read_text('utf-8'))
    if manifest.get('protocol') not in ('v8trace/community-host-source/1', 'v8trace/community-source/1'):
        raise ValueError('Unsupported source manifest')
    rows = manifest.get('files')
    if not isinstance(rows, list) or not rows:
        raise ValueError('Empty source manifest')
    expected, folded = set(), set()
    for row in rows:
        name = row['path']
        relative = PurePosixPath(name)
        if (not isinstance(name, str) or relative.is_absolute() or '\\' in name or ':' in name
                or '..' in relative.parts or relative.as_posix() != name or name in expected
                or name.casefold() in folded or name == 'SOURCE-MANIFEST.json'):
            raise ValueError('Invalid or duplicate manifest path: ' + str(name))
        expected.add(name)
        folded.add(name.casefold())
        source = checked_path(output / name)
        if not source.is_file() or source.stat().st_size != row['bytes'] or digest(source) != row['sha256']:
            raise ValueError('Source manifest identity mismatch: ' + name)
        if relative.parts[0] == 'professional' or source.suffix.lower() in ('.pfx', '.p12', '.exe', '.dll', '.zip', '.pdb'):
            raise ValueError('Private implementation or binary in source export: ' + name)
        if source.suffix.lower() in ('.png', '.ico'):
            magic = b'\x89PNG\r\n\x1a\n' if source.suffix.lower() == '.png' else b'\0\0\1\0'
            with source.open('rb') as stream:
                if not stream.read(len(magic)).startswith(magic):
                    raise ValueError('Invalid reviewed image: ' + name)
        elif re.search(r'github_pat_[A-Za-z0-9_]{20,}|gh[pousr]_[A-Za-z0-9]{20,}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
                       source.read_text('utf-8-sig')):
            raise ValueError('Credential-shaped staged content requires review: ' + name)
    actual = set()
    for source in output.rglob('*'):
        checked_path(source)
        if source.is_file() and source != file:
            actual.add(source.relative_to(output).as_posix())
    if actual != expected:
        raise ValueError('Source manifest inventory mismatch')
    return manifest


def rename_exclusive(source, destination):
    if os.name == 'nt':
        os.rename(source, destination)
    elif sys.platform.startswith('linux'):
        libc = ctypes.CDLL(None, use_errno=True)
        rename = getattr(libc, 'renameat2', None)
        if rename is None:
            raise OSError(errno.ENOTSUP, 'Exclusive atomic directory rename is required')
        rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
        rename.restype = ctypes.c_int
        if rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1):
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), str(destination))
    else:
        raise OSError(errno.ENOTSUP, 'Exclusive atomic directory rename is required')


class ExportTransaction:
    def __init__(self, destination, forbidden_root):
        self.destination = checked_path(destination)
        root = Path(forbidden_root).resolve()
        if self.destination.exists() or self.destination == root or self.destination.is_relative_to(root):
            raise ValueError('A fresh export outside the internal source tree is required')
        self.destination.parent.mkdir(parents=True, exist_ok=True)
        checked_path(self.destination.parent)
        self.stage = Path(tempfile.mkdtemp(prefix='.' + self.destination.name + '.export-', dir=self.destination.parent))
        self.output = self.stage / 'payload'
        self.committed = False

    def __enter__(self):
        return self

    def commit(self):
        validate_manifest(self.output)
        for attempt in range(10):
            checked_path(self.destination)
            try:
                rename_exclusive(self.output, self.destination)
                self.committed = True
                break
            except PermissionError as error:
                if getattr(error, 'winerror', None) not in (5, 32, 33) or attempt == 9:
                    raise
                time.sleep(.05 * (attempt + 1))
        try:
            self.stage.rmdir()
        except OSError as error:
            print(json.dumps(dict(protocol='v8trace/export-cleanup/1', committed=True, stage=str(self.stage), error=repr(error))), file=sys.stderr)

    def __exit__(self, kind, error, traceback):
        if not self.committed:
            report = dict(protocol='v8trace/export-transaction/1', status='failed', destination=str(self.destination),
                          stage=str(self.stage), error=repr(error) if error else 'Commit was not requested',
                          finalCommitted=False, stagePreserved=True)
            try:
                (self.stage / 'FAILURE.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
            except OSError as failure:
                report['receiptError'] = repr(failure)
            print(json.dumps(report, ensure_ascii=True), file=sys.stderr)
            if error is None:
                raise RuntimeError('Export transaction left uncommitted: ' + str(self.stage))
        return False
