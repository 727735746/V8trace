import hashlib
import importlib.util
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location('mcp_fixture', Path(__file__).with_name('mcp_integration_test.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.causal_fixture(root)
capture = root / 'causal-capture'
summary = json.loads((capture / 'summary.json').read_text(encoding='utf-8'))
summary.update(schemaVersion=1, backend='chromium-native')
(capture / 'summary.json').write_text(json.dumps(summary), encoding='utf-8')
calls = capture / 'events/jscall.jsonl'
records = [json.loads(line) for line in calls.read_text(encoding='utf-8').splitlines()]
for row in records:
    row['payload'].update(isolateId='isolate-A', recordingWindow=2, requestId='observed-request-7')
calls.write_text(''.join(json.dumps(row, ensure_ascii=False) + '\n' for row in records), encoding='utf-8')
catalog = capture / 'catalog'
catalog.mkdir()
source = "const marker = 'fixture-source-完整😀';\n".encode()
digest = hashlib.sha256(source).hexdigest()
(capture / 'blobs/sha256' / digest).write_bytes(source)
rows = [dict(scriptId=7, processId=1, isolateId=isolate, url='https://fixture.test/计算.js',
             source=dict(path='blobs/sha256/' + digest, sha256=digest, bytes=len(source), representation='source-utf8'))
        for isolate in ('isolate-A', 'isolate-B')]
(catalog / 'scripts.jsonl').write_text(''.join(json.dumps(row, ensure_ascii=False) + '\n' for row in rows), encoding='utf-8')
