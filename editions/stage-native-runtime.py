import argparse
from pathlib import Path
import shutil

import artifact_binding as a
import workspace_plan as w


def stage(source, output, edition, *, preflight=w.check_output, owners=w.active_owners):
    source, output = a.plain_path(source), a.plain_path(output, exists=False)
    a.require(edition in ('community', 'professional') and not output.exists() and not w.overlaps(source, output), 'Independent fresh native output and explicit edition required')
    receipt_file = Path(str(output) + '.staging.json')
    a.require(not receipt_file.exists(), 'A fresh staging receipt is required')
    descriptor_file, inventory_file = source / 'v8trace-opcode.json', source / 'v8trace-runtime-files.json'
    descriptor, declared = a.read_json(descriptor_file), a.read_json(inventory_file)
    a.require(descriptor.get('edition', 'professional') == edition and declared.get('schemaVersion') == 1 and declared.get('protocol') == 'v8trace/runtime-files/1', 'Source native edition/inventory differs')
    a.require(declared.get('descriptorSha256') == a.file_digest(descriptor_file), 'Source runtime inventory descriptor binding differs')
    names = [a.relative_name(name.replace('\\', '/')) for name in declared['files']]
    a.require(3 <= len(names) <= a.LIMIT and len(names) == len({name.casefold() for name in names}), 'Invalid or aliased native runtime file list')
    a.require({'chrome.exe', 'chrome.dll', 'v8trace-opcode.json'} <= set(names), 'Runtime inventory lacks a native core')
    names = sorted(set(names) | {'v8trace-runtime-files.json'})
    if a.os.name == 'nt':
        a.require(all(len(str(root / name).encode('utf-16le')) // 2 < 260 and len(str((root / name).parent).encode('utf-16le')) // 2 < 248 for root in (source, output) for name in names), 'Native runtime paths exceed the verified legacy file path budget; select shorter independent roots')
    rows = []
    for name in names:
        file = a.plain_path(source / name)
        a.require(file.is_file() and file.is_relative_to(source), 'Runtime inventory leaves its source')
        rows.append(dict(path=name, bytes=file.stat().st_size, sha256=a.file_digest(file)))
    for name, digest in descriptor['binaryHashes'].items():
        name = a.relative_name(name.replace('\\', '/'))
        a.require(name in names and a.file_digest(source / name) == digest, 'Source compiled native core differs')
    for owner in owners():
        a.require(not owner.get('unknownWorkspace') and not any(w.overlaps(source, path) for path in w.owner_roots(owner)), 'A live or unidentified build owner prevents staging this source')
    preflight(output, Path(__file__).resolve().parents[1], sum(row['bytes'] for row in rows) + 134217728)
    output.mkdir(parents=True, exist_ok=False)
    for row in rows:
        destination = output / row['path']
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / row['path'], destination)
        a.require(destination.stat().st_nlink == 1 and a.file_digest(destination) == row['sha256'] == a.file_digest(source / row['path']), 'Native source/copy changed during independent staging')
    observed = a.native_input(output, edition)
    value = dict(protocol='v8trace/native-runtime-staging/1', status='copied-not-browser-validated', edition=edition, source=str(source), output=str(output),
                 selection='declared runtime inventory only; unlisted GN build files not copied', sourceFiles=rows,
                 nativeInputs=observed, hardlinksCreated=False, nativeBrowserValidated=False, remotePublication=False)
    a.write_new(receipt_file, value)
    return value


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Copy an already described GN/browser runtime into a fresh independent runtime directory; no compilation or playback claim.')
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--edition', choices=('community', 'professional'), required=True)
    args = parser.parse_args()
    result = stage(args.source, args.output, args.edition)
    print(a.json.dumps(dict(status=result['status'], output=result['output'], files=len(result['sourceFiles']), nativeBrowserValidated=False)))
