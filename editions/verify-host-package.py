import argparse
import hashlib
import json
from pathlib import Path
import stat
import subprocess


def digest(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--edition', choices=('community', 'professional'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.package.resolve(strict=True)
    manifest_file = root / 'package-manifest.json'
    manifest = json.loads(manifest_file.read_text('utf-8-sig'))
    assert manifest['edition'] == args.edition and manifest['editionProtocol'] == 'v8trace/edition/1'
    assert manifest['runtime'] == 'cpp' and manifest['nodeRequired'] is False
    assert manifest['automaticRuntimeFallback'] is False and manifest['codeSigned'] is False
    assert manifest['browserIncluded'] is False and manifest['releaseState'] == 'host-only-browser-pending'
    listed = set()
    for entry in manifest['files']:
        relative = Path(entry['path'].replace('\\', '/'))
        assert not relative.is_absolute() and '..' not in relative.parts
        name = relative.as_posix()
        assert name not in listed
        listed.add(name)
        file = root / relative
        assert file.resolve(strict=True).is_relative_to(root)
        assert file.stat().st_size == entry['bytes'] and digest(file) == entry['sha256']
        assert file.suffix.lower() not in ('.cpp', '.cc', '.h', '.patch', '.zip', '.pdb')
        assert 'professional' not in relative.parts and file.name.lower() != 'node.exe'
    actual = set()
    for file in root.rglob('*'):
        assert not file.is_symlink()
        if hasattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT'):
            assert not file.stat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT
        if file.is_file() and file != manifest_file:
            actual.add(file.relative_to(root).as_posix())
    assert actual == listed, 'Unlisted/missing files in host candidate'
    for name in ('json', 'ixwebsocket', 'zlib', 'brotli', 'zstd', 'sqlite', 'abseil', 're2', 'v8trace-community-Apache-2.0', 'v8trace-community-NOTICE'):
        assert f'licenses/{name}.txt' in listed
    if args.edition == 'community':
        assert manifest['sourceLicense'] == 'Apache-2.0' and {'LICENSE', 'NOTICE'} <= listed
    else:
        assert manifest['sourceLicense'] == 'proprietary' and 'EDITION-NOTICE.txt' in listed
    runtime = root / 'runtime/v8trace-runtime.exe'
    flags = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0
    version = json.loads(subprocess.check_output([str(runtime), '--version'], creationflags=flags))
    stored = json.loads((root / 'runtime/edition.json').read_text('utf-8-sig'))
    assert stored == version['edition'] and stored['id'] == args.edition
    assert manifest['mcp']['tools'] == (11 if args.edition == 'community' else 16)
    result = dict(protocol='v8trace/host-package-verification/1', edition=args.edition, status='host-candidate-verified', package=str(root), fileCount=len(listed), manifestSha256=digest(manifest_file), uiSha256=digest(root / 'V8Trace.exe'), runtimeSha256=digest(runtime), nativeBrowserValidated=False, wholePackageReleaseEligible=False)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x', encoding='utf-8', newline='\n') as file:
        json.dump(result, file, ensure_ascii=False, indent=2)
        file.write('\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
