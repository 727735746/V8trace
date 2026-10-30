import argparse
import hashlib
import http.server
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading


def require(value, message):
    if not value:
        raise AssertionError(message)


def rows(file):
    with file.open('rb') as stream:
        return [json.loads(line) for line in stream if line.strip()]


def verify(runtime, assets, chrome, evidence):
    evidence.mkdir(parents=True, exist_ok=False)
    identity = json.loads(subprocess.check_output([str(runtime), '--version']))
    require(identity['edition']['id'] == 'community', 'An actual Community runtime is required')
    marker = '完整参数-' + chr(0xd800) + '-TAIL'
    body = b'COMPLETE_HTTP_BODY\0\xff' + '完整中文正文'.encode()
    script = ('function communityEcho(value){return value;}\n'
        'const fullValue=communityEcho(' + json.dumps(marker) + ');\n'
        'void navigator.languages;document.body.setAttribute("data-proof","FULL_DOM_VALUE");\n'
        'localStorage.setItem("capture-proof","FULL_STORAGE");document.cookie="capture-proof=FULL_COOKIE; path=/";\n'
        'new URL("https://fixture.test/?value=COMPLETE_NATIVE_VALUE");\n'
        'fetch("/body",{method:"POST",body:"FULL_UPLOAD"}).then(r=>r.arrayBuffer()).then(()=>{console.log("COMMUNITY_PRESET_DONE",fullValue);setTimeout(()=>{throw new Error("COMMUNITY_EXCEPTION_PROOF")},50);});\n').encode()
    received = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *unused):
            pass

        def do_GET(self):
            page = self.path == '/'
            data = b'<!doctype html><html><body><script src="/community-fixture.js"></script></body></html>' if page else script
            self.send_response(200)
            self.send_header('Content-Type', 'text/html' if page else 'text/javascript;charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            received.append(self.rfile.read(int(self.headers.get('Content-Length', '0'))))
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    report = dict(protocol='v8trace/community-live-environment-presets/1', conclusion='running',
        edition='community', runtimeSha256=hashlib.sha256(runtime.read_bytes()).hexdigest(),
        scope='Actual packaged preset defaults plus local URL, target script and a 25-second window; Chromium background traffic remains observed', cases=[])
    temporary = tempfile.TemporaryDirectory(prefix='v8trace-community-presets-')
    root = Path(temporary.name)
    try:
        for preset in ['env-protocol', 'env-target-detail']:
            output = root / preset
            options = dict(preset=preset, url='http://127.0.0.1:' + str(server.server_port) + '/',
                chromePath=str(chrome), output=str(output), durationSeconds=25, headless=True, disableExtensions=True)
            if preset == 'env-target-detail':
                options['detailScriptInclude'] = 'community-fixture.js'
            config = root / (preset + '.json')
            config.write_text(json.dumps(options), encoding='utf-8')
            process = subprocess.run([str(runtime), 'capture', '--config', str(config), '--assets', str(assets)],
                capture_output=True, timeout=200)
            (evidence / (preset + '.stdout.log')).write_bytes(process.stdout)
            (evidence / (preset + '.stderr.log')).write_bytes(process.stderr)
            require((output / 'summary.json').is_file(), preset + ' did not produce summary.json')
            summary = json.loads((output / 'summary.json').read_bytes())
            shutil.copyfile(output / 'summary.json', evidence / (preset + '-summary.json'))
            require(process.returncode == 0 and summary['captureConclusion'] != 'failed', preset + ' capture failed')
            require(not process.stdout.strip(), 'Capture CLI printed business logs instead of recording files')
            require(summary['integrityConclusion'] == 'complete' and summary['criticalGapCount'] == 0,
                preset + ' is partial: ' + json.dumps(summary.get('gaps'))[:4000])
            for category in ['env', 'http', 'script', 'console', 'exception']:
                require(summary['counts'].get(category, 0) > 0, preset + ' missing selected channel: ' + category)
            require('COMMUNITY_PRESET_DONE' in (output / 'events/console.jsonl').read_text('utf-8'), 'Fixture did not finish')
            require('COMMUNITY_EXCEPTION_PROOF' in (output / 'events/exception.jsonl').read_text('utf-8'), 'Unhandled exception value absent')
            resources = rows(output / 'catalog/resources.jsonl')
            matches = [row for row in resources if row.get('request', {}).get('url', '').endswith('/body')]
            require(any(row.get('body') and (output / row['body']['path']).read_bytes() == body for row in matches), 'Exact full HTTP response absent')
            require(any(row.get('postBody') and (output / row['postBody']['path']).read_bytes() == b'FULL_UPLOAD' for row in matches), 'Exact HTTP upload absent')
            require((output / 'blobs/sha256' / hashlib.sha256(script).hexdigest()).is_file(), 'Exact target script bytes absent')
            http_records = [row['payload'] for row in rows(output / 'events/http.jsonl')]
            require(any(row.get('producerEdition') == 'community' for row in http_records), 'Actual Community native producer marker absent')
            require(not any(summary['counts'].get(channel, 0) for channel in ['opcode', 'wasm', 'vmp', 'causality']), 'Professional producer present')
            if preset == 'env-target-detail':
                calls = [row['payload'] for row in rows(output / 'events/jscall.jsonl')]
                require(any(arg.get('value') == marker for row in calls for arg in row.get('arguments', [])), 'Full JSCall argument absent')
                native = [row['payload'] for row in rows(output / 'events/native.jsonl')]
                require(any('COMPLETE_NATIVE_VALUE' in json.dumps(row) for row in native), 'Full Native argument absent')
            report['cases'].append(dict(preset=preset, conclusion='PASS', counts=summary['counts'], criticalGaps=0))
        require(received == [b'FULL_UPLOAD', b'FULL_UPLOAD'], 'Actual protocol fixture uploads differ')
        report['conclusion'] = 'PASS'
    except BaseException as error:
        report.update(conclusion='FAIL', error=repr(error))
        if 'output' in locals() and output.is_dir():
            shutil.copytree(output, evidence / ('failure-' + preset), ignore=shutil.ignore_patterns('browser-profile'))
        raise
    finally:
        temporary.cleanup()
        report['temporaryFixturesRemoved'] = not root.exists()
        server.shutdown()
        server.server_close()
        thread.join(timeout=10)
        (evidence / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ['runtime', 'assets', 'chrome', 'evidence']:
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(verify(args.runtime.resolve(strict=True), args.assets.resolve(strict=True),
        args.chrome.resolve(strict=True), args.evidence.resolve())))
