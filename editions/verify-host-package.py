import argparse
import hashlib
import json
from pathlib import Path
import stat
import importlib.util

spec=importlib.util.spec_from_file_location("artifact_binding",Path(__file__).with_name("artifact_binding.py"))
binding=importlib.util.module_from_spec(spec)
spec.loader.exec_module(binding)
require=binding.require
import subprocess


def digest(file):
    return binding.file_digest(file)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--edition', choices=('community', 'professional'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--binding', type=Path)
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    root = binding.plain_path(args.package)
    manifest_file = root / 'package-manifest.json'
    manifest = json.loads(manifest_file.read_text('utf-8-sig'))
    require(manifest['edition'] == args.edition and manifest['editionProtocol'] == 'v8trace/edition/1', 'Host package contract failed')
    require(manifest['runtime'] == 'cpp' and manifest['nodeRequired'] is False, 'Host package contract failed')
    require(manifest['automaticRuntimeFallback'] is False and manifest['codeSigned'] is False, 'Host package contract failed')
    require(manifest['browserIncluded'] is False and manifest['releaseState'] == 'host-only-browser-pending', 'Host package contract failed')
    listed = set()
    for entry in manifest['files']:
        relative = Path(entry['path'].replace('\\', '/'))
        require(not relative.is_absolute() and '..' not in relative.parts, 'Host package contract failed')
        name = relative.as_posix()
        require(name not in listed, 'Host package contract failed')
        listed.add(name)
        file = root / relative
        require(file.resolve(strict=True).is_relative_to(root), 'Host package contract failed')
        require(file.stat().st_size == entry['bytes'] and digest(file) == entry['sha256'], 'Host package contract failed')
        require(file.suffix.lower() not in ('.cpp', '.cc', '.h', '.patch', '.zip', '.pdb'), 'Host package contract failed')
        require('professional' not in relative.parts and file.name.lower() != 'node.exe', 'Host package contract failed')
    actual = set()
    for file in root.rglob('*'):
        require(not file.is_symlink(), 'Host package contract failed')
        if hasattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT'):
            require(not file.stat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT, 'Host package contract failed')
        if file.is_file() and file != manifest_file:
            actual.add(file.relative_to(root).as_posix())
    require(actual == listed, 'Unlisted/missing files in host candidate')
    for name in ('json', 'ixwebsocket', 'zlib', 'brotli', 'zstd', 'sqlite', 'abseil', 're2', 'v8trace-community-Apache-2.0', 'v8trace-community-NOTICE'):
        require(f'licenses/{name}.txt' in listed, 'Host package contract failed')
    if args.edition == 'community':
        require(manifest['sourceLicense'] == 'Apache-2.0' and {'LICENSE', 'NOTICE'} <= listed, 'Host package contract failed')
    else:
        require(manifest['sourceLicense'] == 'proprietary' and 'EDITION-NOTICE.txt' in listed, 'Host package contract failed')
    runtime = root / 'runtime/v8trace-runtime.exe'
    flags = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0
    version = json.loads(subprocess.check_output([str(runtime), '--version'], creationflags=flags))
    stored = json.loads((root / 'runtime/edition.json').read_text('utf-8-sig'))
    require(stored == version['edition'] and stored['id'] == args.edition, 'Host package contract failed')
    require(manifest['mcp']['tools'] == (11 if args.edition == 'community' else 16), 'Host package contract failed')
    bound = None
    if manifest.get('artifactBindingProtocol') == 'v8trace/product-binding/1':
        name = manifest.get('artifactBindingFileName', '')
        require(Path(name).name == name and name and '/' not in name and '\\' not in name, 'Invalid external binding name')
        path = args.binding or root.parent / name
        observed = binding.verify_binding(path, source=args.source or False)
        require(Path(observed['package']) == root and observed['packageBatchId'] == manifest['packageBatchId'], 'Host package binding root/batch differs')
        bound = binding.reference(path)
    elif args.binding:
        raise ValueError('Legacy package does not declare the new product binding protocol')
    result = dict(protocol='v8trace/host-package-verification/1', edition=args.edition, status='host-candidate-verified', package=str(root), fileCount=len(listed), manifestSha256=digest(manifest_file), uiSha256=digest(root / 'V8Trace.exe'), runtimeSha256=digest(runtime), nativeBrowserValidated=False, wholePackageReleaseEligible=False)
    result['productBinding']=bound
    result['productBindingVerified']=bound is not None
    if bound is None:result['status']='legacy-host-files-verified-unbound'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x', encoding='utf-8', newline='\n') as file:
        json.dump(result, file, ensure_ascii=False, indent=2)
        file.write('\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
