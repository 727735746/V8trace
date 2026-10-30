import argparse
import hashlib
import http.server
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
from urllib.parse import urlparse


def digest(file):
    value = hashlib.sha256()
    with file.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1048576), b''):
            value.update(chunk)
    return value.hexdigest()


def verify(args):
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    fixtures = args.fixtures.resolve(strict=True)
    script = Path(__file__).with_name('media-fixture.js').read_bytes()
    reports = []
    lock = threading.Lock()
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *unused):
            pass
        def do_GET(self):
            route = urlparse(self.path).path
            if route == '/':
                data = b'<!doctype html><body><script src="/media-fixture.js"></script>'; mime = 'text/html'
            elif route == '/media-fixture.js':
                data = script; mime = 'text/javascript;charset=utf-8'
            elif route in ['/fixture.mp4', '/fixture.webm']:
                data = (fixtures / route[1:]).read_bytes(); mime = 'video/mp4' if route.endswith('mp4') else 'video/webm'
            else:
                self.send_error(404); return
            start, end, size = 0, len(data) - 1, len(data)
            range_value = self.headers.get('Range', '')
            if range_value.startswith('bytes='):
                first, last = range_value[6:].split('-', 1)
                start, end = int(first or 0), min(int(last) if last else end, end)
                self.send_response(206)
                self.send_header('Content-Range', 'bytes ' + str(start) + '-' + str(end) + '/' + str(size))
            else:
                self.send_response(200)
            self.send_header('Content-Type', mime); self.send_header('Accept-Ranges', 'bytes')
            self.send_header('Content-Length', str(end - start + 1)); self.end_headers()
            self.wfile.write(data[start:end + 1])
        def do_POST(self):
            body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            with lock:
                reports.append(json.loads(body))
            self.send_response(204); self.send_header('Content-Length', '0'); self.end_headers()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    temporary = tempfile.TemporaryDirectory(prefix='v8trace-media-capture-')
    root = Path(temporary.name)
    result = dict(protocol='v8trace/media-runtime-validation/1', conclusion='running', edition=args.edition,
        runtimeSha256=digest(args.runtime),
        binaryHashes={name: digest(args.chrome.parent / name) for name in
            ['chrome.exe', 'chrome.dll', 'chrome_elf.dll', 'v8_context_snapshot.bin']},
        fixtureScriptSha256=hashlib.sha256(script).hexdigest(),
        fixtureHashes={name: digest(fixtures / name) for name in ['fixture.mp4', 'fixture.webm']},
        descriptorSha256=digest(args.chrome.parent / 'v8trace-opcode.json'), cases=[])
    try:
        identity = json.loads(subprocess.check_output([str(args.runtime), '--version']))
        if identity['edition']['id'] != args.edition:
            raise RuntimeError('Compiled runtime edition differs')
        for name, preset in [('baseline', 'capture-baseline'), ('detailed', 'env-target-detail')]:
            output = root / name
            options = dict(preset=preset, url='http://127.0.0.1:' + str(server.server_port) + '/', chromePath=str(args.chrome),
                output=str(output), durationSeconds=25, headless=not args.headful, disableExtensions=True, redirectBodies=True,
                maxBytes=268435456, maxEvents=2000000, traceFlushTimeout=120000)
            if name == 'detailed':
                options['detailScriptInclude'] = 'media-fixture.js'
            config = root / (name + '.json'); config.write_text(json.dumps(options), encoding='utf-8')
            with lock:
                before = len(reports)
            process = subprocess.run([str(args.runtime), 'capture', '--config', str(config), '--assets', str(args.assets)], capture_output=True, timeout=220)
            (evidence / (name + '.stdout.log')).write_bytes(process.stdout)
            (evidence / (name + '.stderr.log')).write_bytes(process.stderr)
            if not (output / 'summary.json').is_file():
                raise RuntimeError('Media capture produced no summary')
            summary = json.loads((output / 'summary.json').read_bytes())
            shutil.copyfile(output / 'summary.json', evidence / (name + '-summary.json'))
            with lock:
                observed = reports[before:]
            if not observed or not observed[-1].get('ok'):
                raise RuntimeError('Actual media playback/encoding failed: ' + json.dumps(observed)[-6000:])
            if process.returncode or summary['integrityConclusion'] != 'complete' or summary['criticalGapCount'] != 0:
                raise RuntimeError('Capture during video playback was not complete: ' + json.dumps(summary.get('gaps'))[:6000])
            if name == 'detailed' and not all(summary['counts'].get(channel, 0) for channel in ['jscall', 'native']):
                raise RuntimeError('Detailed call capture did not run during playback')
            result['cases'].append(dict(mode=name, conclusion='PASS', counts=summary['counts'], media=observed[-1], criticalGaps=0))
        result['conclusion'] = 'PASS'
    except BaseException as error:
        result.update(conclusion='FAIL', error=repr(error))
        if 'output' in locals() and output.is_dir():
            shutil.copytree(output, evidence / ('failure-' + name), ignore=shutil.ignore_patterns('browser-profile'))
        raise
    finally:
        temporary.cleanup(); result['temporaryFixturesRemoved'] = not root.exists()
        server.shutdown(); server.server_close(); thread.join(timeout=10)
        (evidence / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for key in ['runtime', 'assets', 'chrome', 'fixtures', 'evidence']:
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--edition', choices=['community', 'professional'], required=True)
    parser.add_argument('--headful', action='store_true')
    args = parser.parse_args()
    for key in ['runtime', 'assets', 'chrome', 'fixtures']:
        setattr(args, key, getattr(args, key).resolve(strict=True))
    print(json.dumps(verify(args)))
