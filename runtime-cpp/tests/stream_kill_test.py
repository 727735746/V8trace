import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--runtime', required=True)
parser.add_argument('--evidence')
args = parser.parse_args()
runtime = str(Path(args.runtime).resolve())
def digest(file):
    value = hashlib.sha256()
    with file.open('rb') as data:
        for block in iter(lambda: data.read(1048576), b''): value.update(block)
    return value.hexdigest()
report = {'test': 'owned-stream-producer-hard-stop', 'passed': False}
with tempfile.TemporaryDirectory(prefix='v8trace-stream-kill-') as temporary:
    root = Path(temporary).resolve()
    source = root / 'source'; source.mkdir(); data = source / 'data.bin'
    block = random.Random(7).randbytes(1048576)
    with data.open('wb') as output:
        for _ in range(64): output.write(block)
    expected = digest(data)
    spool = root / 'spool'
    process = subprocess.Popen([runtime, 'stream-pack', '--capture', str(source), '--spool', str(spool)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and process.poll() is None:
            if (spool / 'chunks/00000000000000000001/chunk.json').is_file(): break
            time.sleep(.005)
        assert process.poll() is None, 'producer completed/exited before the hard-stop checkpoint'
        assert not (spool / 'commit').exists(), 'final commit appeared before hard stop'
        process.kill()
        process.communicate(timeout=20)
        assert process.returncode != 0
        assert digest(data) == expected
        assert (spool / 'chunks/00000000000000000001/payload.zst').is_file()
        unpack = subprocess.run([runtime, 'stream-unpack', '--spool', str(spool), '--output', str(root / 'restore')], capture_output=True, timeout=30)
        assert unpack.returncode != 0 and not (root / 'restore').exists()
        report.update(passed=True, exitCode=process.returncode, sourceBytes=data.stat().st_size, sourceSha256=expected, finalCommit=False, sealedChunks=len(list((spool / 'chunks').glob('[0-9]' * 20))), restoreRejected=True, sourceRetained=True)
    finally:
        if process.poll() is None: process.kill(); process.communicate(timeout=20)
report['temporaryFixturesRemoved'] = not root.exists()
if args.evidence:
    Path(args.evidence).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report, ensure_ascii=False))
