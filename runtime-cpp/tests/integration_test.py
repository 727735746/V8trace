import argparse
import base64
import gzip
import http.server
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest


PARSER = argparse.ArgumentParser()
PARSER.add_argument('--runtime', required=True)
PARSER.add_argument('--assets', required=True)
PARSER.add_argument('--evidence')
PARSER.add_argument('--chrome')
ARGS, REMAINING = PARSER.parse_known_args()
RUNTIME = str(Path(ARGS.runtime).resolve())
ASSETS = str(Path(ARGS.assets).resolve())
REPORT = []


def encode(value):
    return json.dumps(value, ensure_ascii=True, separators=(',', ':')).encode()


def write_json(file, value):
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_bytes(encode(value))


def invoke(*arguments, timeout=60):
    result = subprocess.run([RUNTIME, *map(str, arguments)], capture_output=True, timeout=timeout)
    if result.returncode:
        raise AssertionError((result.returncode, result.stdout.decode('utf-8', 'replace')[-2000:], result.stderr.decode('utf-8', 'replace')))
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


def port():
    with socket.socket() as server:
        server.bind(('127.0.0.1', 0))
        return server.getsockname()[1]


def frame(connection, header, payload=b''):
    raw = encode(dict(header, wireBytes=len(payload)))
    connection.sendall(b'V8T1' + struct.pack('!I', len(raw)) + raw + payload)


def exact(connection, length):
    out = bytearray()
    while len(out) < length:
        chunk = connection.recv(length - len(out))
        if not chunk:
            raise ConnectionError('closed')
        out.extend(chunk)
    return bytes(out)


def receive_frame(connection):
    prefix = exact(connection, 8)
    if prefix[:4] != b'V8T1':
        raise AssertionError(prefix)
    header = json.loads(exact(connection, struct.unpack('!I', prefix[4:])[0]))
    return header, exact(connection, header['wireBytes'])


class Integration(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='v8trace-cpp-integration-'))
        self.processes = []

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream:
                    stream.close()
        resolved = self.root.resolve()
        if resolved.parent != Path(tempfile.gettempdir()).resolve() or not resolved.name.startswith('v8trace-cpp-integration-'):
            raise AssertionError('temporary directory identity changed')
        shutil.rmtree(resolved)

    def server(self, options=None):
        listen_port = port()
        command = [RUNTIME, 'receive', '--root', str(self.root / 'received'), '--port', str(listen_port)] + (options or [])
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.processes.append(process)
        ready = json.loads(process.stdout.readline())
        self.assertEqual(ready['type'], 'listening')
        return process, listen_port

    def fixture(self):
        events = []
        sequences = {}

        def browser(channel, kind, **fields):
            n = sequences.get(channel, 0) + 1
            sequences[channel] = n
            record = dict(schema='v8trace/browser/1', producer='v8', producerSeq=str(n), channel=channel,
                          kind=kind, isolateId='i1', isolateAddress='a1', realmId=1, contextId=1, **fields)
            events.append(dict(name='V8TraceBrowser', cat='disabled-by-default-v8.' + channel,
                               pid=77, tid=9, ts=10 + len(events), args=dict(record=encode(record).decode())))

        source = '// 中文\x00 ' + chr(0xd800) + '\nfunction f(x) { return x; }'
        browser('script', 'source', source=source, url='https://fixture.test/target.js', scriptId=5, isModule=False)
        browser('env', 'hook-installed', url='https://fixture.test/')
        browser('env', 'hook', url='https://fixture.test/', hookRecord=encode(dict(kind='ready', producerSeq=1)).decode())
        for n, kind in [(2, 'enter'), (3, 'return')]:
            browser('env', 'hook', url='https://fixture.test/', hookRecord=encode(dict(kind=kind, producerSeq=n, spanId=1, parentSpanId=None, binding='Math.random', operation='call', value=0.25)).decode())
        browser('console', 'api', typeCode=0, arguments=[dict(type='string', value='日志' + chr(0xdfff))])
        browser('exception', 'unhandled', exception=dict(type='string', value='fixture-error'))
        payload = b'body\x00\xff' + '中文正文'.encode() * 100
        compressed = gzip.compress(payload, mtime=0)
        post = bytes(range(256)) * 4
        browser('http', 'request', requestId='n1', layer='network', url='https://fixture.test/target.js', method='POST', uploadStreamId='s1', requestBodyPresent=True, headers={'Cookie': 'full=1'})
        browser('http', 'upload-init', requestId='u1', uploadStreamId='s1')
        for offset in range(0, len(post), 200):
            chunk = post[offset:offset + 200]
            browser('http', 'body-chunk', requestId='u1', direction='request', offset=str(offset), bytes=len(chunk), base64=base64.b64encode(chunk).decode())
        browser('http', 'request-body-end', requestId='u1', complete=True, bytes=str(len(post)))
        browser('http', 'response', requestId='n1', status=200, mimeType='text/javascript', bodyRepresentation='encoded-network-bytes', headers={'content-encoding': 'gzip', 'Set-Cookie': 'full=2'})
        browser('http', 'body-chunk', requestId='n1', direction='response', offset='0', bytes=len(compressed), base64=base64.b64encode(compressed).decode())
        browser('http', 'complete', requestId='n1', success=True, pipe=False, bytes=str(len(compressed)))
        for channel in ('jscall', 'native'):
            for seq, phase in ((2, 'return'), (1, 'entry')):
                record = dict(schema='v8trace/detail/1', channel=channel, producerSeq=seq, phase=phase,
                              isolateId='i1', contextId=1, callId='1', scriptId=5, functionName='f',
                              arguments=[dict(type='string', value='value' + chr(0xd800)), dict(type='bigint', value='18446744073709551615')],
                              result=dict(type='number', value=-0.0))
                events.append(dict(name='V8TraceDetail', cat='disabled-by-default-v8.' + channel,
                                   pid=77, tid=9, ts=100 + seq, args=dict(record=encode(record).decode())))
        wasm = dict(schema='v8trace/detail/1', channel='wasm', producerSeq=1, phase='instruction', engine='DrumBrake',
                    isolateId='i1', contextId=1, pc=0, functionIndex=0, opcode=1, opcodeName='I64Const', state=[dict(index=0, type='i64', rawHex='ffffffffffffffff')])
        events.append(dict(name='V8TraceDetail', cat='disabled-by-default-v8.wasm', pid=77, tid=9, ts=111, args=dict(record=wasm)))
        for seq, phase in ((1, 'entry'), (2, 'exit')):
            record = dict(schema='v8trace/opcode/1', engine='Ignition', phase=phase, producerSeq=seq,
                          isolateId='i1', contextId=1, dispatchOffset=0, instructionOffset=0, opcode='LdaZero', bytes=[12])
            events.append(dict(name='V8TraceOpcode', cat='disabled-by-default-v8.opcode', pid=77, tid=9, ts=200 + seq, args=dict(record=record)))
        trace = self.root / 'raw.json'
        write_json(trace, dict(traceEvents=events, metadata=dict(trace_processor_stats=dict(traced_final_flush_succeeded=1))))
        options = self.root / 'options.json'
        write_json(options, dict(url='https://fixture.test/', output=str(self.root / 'capture'), transport='native',
                                scripts=True, http=True, hooks=True, consoleEvents=True, exceptionEvents=True,
                                jsCallDetail=True, nativeDetail=True, wasm=True, opcode=True, correlations=True,
                                maxBytes=16777216, maxEvents=1000))
        return trace, options, source, payload, post

    def test_native_import_full_values_and_export(self):
        trace, options, source, payload, post = self.fixture()
        summary = invoke('import', '--trace', trace, '--config', options, '--assets', ASSETS)[-1]
        self.assertEqual(summary['integrityConclusion'], 'complete', summary['gaps'])
        self.assertEqual(summary['counts']['jscall'], 2)
        self.assertEqual(summary['counts']['wasm'], 1)
        capture = self.root / 'capture'
        scripts = [json.loads(line) for line in (capture / 'catalog/scripts.jsonl').read_bytes().splitlines()]
        self.assertEqual((capture / scripts[0]['sourceUtf16']['path']).read_bytes(), source.encode('utf-16-le', 'surrogatepass'))
        resources = [json.loads(line) for line in (capture / 'catalog/resources.jsonl').read_bytes().splitlines()]
        self.assertEqual(len(resources), 1)
        self.assertEqual((capture / resources[0]['body']['path']).read_bytes(), payload)
        self.assertEqual((capture / resources[0]['postBody']['path']).read_bytes(), post)
        self.assertEqual(resources[0]['request']['headers']['Cookie'], 'full=1')
        output = self.root / 'export'
        result = invoke('export', '--capture', capture, '--output', output)[-1]
        self.assertEqual(result['captureIntegrity'], 'complete')
        self.assertTrue((output / '交接说明.md').read_text(encoding='utf-8').startswith('# 采集证据交接'))
        self.assertEqual(list(capture.glob('native-output/*-cpp-*')), [])
        REPORT.append(dict(test=self.id(), conclusion='passed', events=summary['eventCount'], sourceUtf16Sha256=scripts[0]['sourceUtf16']['sha256'], payloadSha256=hashlib.sha256(payload).hexdigest()))

    def test_worker_no_node_prompt_and_shutdown(self):
        environment = dict(os.environ, NODE_OPTIONS='--v8trace-must-not-use-node')
        if os.name == 'nt':
            environment['PATH'] = str(Path(os.environ['WINDIR']) / 'System32')
        process = subprocess.Popen([RUNTIME, '--worker', '--assets', ASSETS], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
        self.processes.append(process)
        ready = json.loads(process.stdout.readline())
        self.assertEqual(ready['runtime'], 'cpp')
        self.assertIn('tcp', ready)
        self.assertEqual(ready['recordingControl']['protocol'], 'v8trace/recording/1')
        process.stdin.write(encode(dict(id='1', command='case-prompt', templateId=ready['casePromptTemplates'][0]['id'], fields=dict(goal='中文验证'))) + b'\n')
        process.stdin.flush()
        response = json.loads(process.stdout.readline())
        self.assertEqual(response['type'], 'case-prompt')
        self.assertIn('中文验证', response['text'])
        process.stdin.write(b'{"id":"2","command":"shutdown"}\n')
        process.stdin.flush()
        self.assertEqual(json.loads(process.stdout.readline())['type'], 'stopping')
        process.wait(timeout=10)
        self.assertEqual(process.returncode, 0)

    def test_tcp_multichunk_binary_and_compression(self):
        source = self.root / 'source'
        source.mkdir()
        (source / 'events').mkdir()
        data = encode(dict(value='完整中文日志', arguments=['all'] * 50)) * 20000 + os.urandom(1024)
        (source / 'events' / '日志.jsonl').write_bytes(data)
        (source / 'binary.bin').write_bytes(bytes(range(256)) * 30000)
        gaps = b''.join(encode(dict(gapSeq=i + 1, reason='完整缺口', details=dict(index=i, value='中文' * 35000 if i == 900 else 'all'))) + b'\n' for i in range(1500))
        (source / 'gaps.jsonl').write_bytes(gaps)
        (source / 'empty').touch()
        (source / 'browser-profile').mkdir()
        (source / 'browser-profile' / 'do-not-transfer').write_text('profile')
        server, listen_port = self.server(['--once'])
        rows = invoke('send', '--capture', source, '--port', listen_port, timeout=90)
        receipt = rows[-1]
        server.wait(timeout=15)
        destination = Path(receipt['destination'])
        self.assertEqual((destination / 'events' / '日志.jsonl').read_bytes(), data)
        self.assertEqual((destination / 'gaps.jsonl').read_bytes(), gaps)
        self.assertEqual(hashlib.sha256((destination / 'binary.bin').read_bytes()).hexdigest(), hashlib.sha256((source / 'binary.bin').read_bytes()).hexdigest())
        self.assertFalse((destination / 'browser-profile').exists())
        self.assertTrue((source / 'binary.bin').exists())
        self.assertLess(receipt['compressedPayloadBytesThisRun'], receipt['sourceBytes'] // 10)
        REPORT.append(dict(test=self.id(), conclusion='passed', sourceBytes=receipt['sourceBytes'], compressedBytes=receipt['compressedPayloadBytesThisRun'], files=receipt['files']))

    def test_tcp_resume_and_commit_ack_loss(self):
        source = self.root / 'source'
        source.mkdir()
        (source / 'data.bin').write_bytes(os.urandom(6 * 1024 * 1024))
        server, listen_port = self.server()
        proxy = socket.socket()
        proxy.bind(('127.0.0.1', 0))
        proxy.listen(4)
        proxy_port = proxy.getsockname()[1]
        cuts = []

        def relay():
            for attempt in range(2):
                client, _ = proxy.accept()
                with client, socket.create_connection(('127.0.0.1', listen_port), timeout=15) as remote:
                    client.settimeout(15)
                    while True:
                        header, payload = receive_frame(client)
                        frame(remote, header, payload)
                        ack, data = receive_frame(remote)
                        if attempt == 0 and header['op'] == 'chunk':
                            cuts.append(ack['offset'])
                            break
                        frame(client, ack, data)
                        if header['op'] == 'commit':
                            break
            proxy.close()

        thread = threading.Thread(target=relay, daemon=True)
        thread.start()
        receipt = invoke('send', '--capture', source, '--port', proxy_port, timeout=90)[-1]
        thread.join(timeout=10)
        self.assertFalse(thread.is_alive())
        self.assertEqual(cuts, [4 * 1024 * 1024])
        self.assertEqual(receipt['attempts'], 2)
        self.assertEqual((Path(receipt['destination']) / 'data.bin').read_bytes(), (source / 'data.bin').read_bytes())
        repeated = invoke('send', '--capture', source, '--port', listen_port)[-1]
        self.assertEqual(repeated['compressedPayloadBytesThisRun'], 0)
        REPORT.append(dict(test=self.id(), conclusion='passed', resumedOffset=cuts[0], attempts=receipt['attempts'], repeatPayloadBytes=repeated['compressedPayloadBytesThisRun']))

    def test_tcp_rejects_traversal_and_bad_hash(self):
        server, listen_port = self.server()
        for filename, digest in [('../escape', 'a' * 64), ('x.bin', 'b' * 64)]:
            manifest = encode([dict(path=filename, bytes=1, sha256=digest)])
            with socket.create_connection(('127.0.0.1', listen_port), timeout=10) as client:
                frame(client, dict(op='hello', protocol='v8trace/tcp/1', id='11111111-1111-4111-8111-111111111111', token='', manifestSha256=hashlib.sha256(manifest).hexdigest()), manifest)
                ack, _ = receive_frame(client)
                if filename.startswith('..'):
                    self.assertEqual(ack['op'], 'error')
                else:
                    self.assertEqual(ack['op'], 'ack')
                    frame(client, dict(op='file', path=filename))
                    ack, _ = receive_frame(client)
                    self.assertEqual(ack['offset'], 0)
                    frame(client, dict(op='chunk', offset=0, rawBytes=1, codec='zstd', sha256='b' * 64), b'not-zstd')
                    ack, _ = receive_frame(client)
                    self.assertEqual(ack['op'], 'error')
        self.assertFalse((self.root / 'escape').exists())

    def test_tcp_authentication_and_portable_paths(self):
        secret = self.root / 'token.txt'
        secret.write_text('fixture-token-at-least-16-bytes', encoding='utf-8')
        server, listen_port = self.server(['--token-file', str(secret)])
        with socket.create_connection(('127.0.0.1', listen_port), timeout=10) as client:
            manifest = encode([])
            frame(client, dict(op='hello', protocol='v8trace/tcp/1', id='11111111-1111-4111-8111-111111111111', token='wrong', manifestSha256=hashlib.sha256(manifest).hexdigest()), manifest)
            self.assertEqual(receive_frame(client)[0]['op'], 'error')
        for index, names in enumerate([['CON.txt'], ['dir/x.'], ['dir/a:stream'], ['case.txt', 'CASE.txt']]):
            manifest = encode([dict(path=name, bytes=0, sha256=hashlib.sha256(b'').hexdigest()) for name in names])
            transfer_id = str(index).zfill(8) + '-1111-4111-8111-111111111111'
            with socket.create_connection(('127.0.0.1', listen_port), timeout=10) as client:
                frame(client, dict(op='hello', protocol='v8trace/tcp/1', id=transfer_id, token=secret.read_text(), manifestSha256=hashlib.sha256(manifest).hexdigest()), manifest)
                self.assertEqual(receive_frame(client)[0]['op'], 'error')
        source = self.root / 'source'
        source.mkdir()
        (source / '完整日志.txt').write_text('full token-authenticated log', encoding='utf-8')
        receipt = invoke('send', '--capture', source, '--port', listen_port, '--token-file', secret)[-1]
        self.assertEqual(receipt['conclusion'], 'transferred-and-sha256-verified')
        self.assertNotIn(secret.read_text(), json.dumps(receipt))

    def chrome_capture(self, mode, opcode=False):
        marker = '完整参数-' + chr(0xd800) + '-TAIL'
        body = b'HTTP-COMPLETE\x00\xff' + '正文完整'.encode()
        script = ('function echoFull(value){return value;}\n'
                  'const value=echoFull(' + json.dumps(marker) + ');\n'
                  'document.body.setAttribute("data-v8trace", "complete");\n'
                  'new URL("https://fixture.test/full?value=COMPLETE_NATIVE_VALUE");\n'
                  'const wasm=new WebAssembly.Instance(new WebAssembly.Module(Uint8Array.from([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,127,3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,1,7,0,32,0,32,1,106,11])));\n'
                  'wasm.exports.add(4,5);\n'
                  'fetch("/body",{method:"POST",body:"FULL_UPLOAD"}).then(r=>r.arrayBuffer()).then(()=>console.log("CPP_BROWSER_DONE",value));\n')

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                data = b'<html><body><script src="/fixture.js"></script></body></html>' if self.path == '/' else script.encode()
                self.send_response(200)
                self.send_header('Content-Type', 'text/html' if self.path == '/' else 'text/javascript')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_POST(self):
                self.rfile.read(int(self.headers.get('Content-Length', '0')))
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_):
                pass

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        assets = self.root / 'assets'
        assets.mkdir()
        for name in ('scenario_presets.json', 'case_prompt_templates.json'):
            shutil.copyfile(Path(ASSETS) / name, assets / name)
        packaged_hooks = Path(ASSETS) / 'diagnostic-hooks.js'
        if packaged_hooks.is_file():
            shutil.copyfile(packaged_hooks, assets / 'diagnostic-hooks.js')
        else:
            hooks = (Path(ASSETS) / 'hooks.js').read_text(encoding='utf-8')
            (assets / 'diagnostic-hooks.js').write_text(hooks[hooks.index('function installDiagnosticHooks('):hooks.index('\nfunction createHookSource(')], encoding='utf-8')
        options = dict(url='http://127.0.0.1:' + str(server.server_port) + '/', chromePath=str(Path(ARGS.chrome).resolve()),
                       output=str(self.root / 'capture'), transport=mode, durationSeconds=4, headless=True, disableExtensions=True,
                       scripts=True, http=True, hooks=True, consoleEvents=True, exceptionEvents=True,
                       cpuProfile=False, contextEvents=(mode == 'cdp'), rawCdp=(mode == 'cdp'), nativeTimeline=False,
                       maxBytes=1073741824, maxEvents=200000, traceFlushTimeout=90000)
        if mode == 'native':
            options.update(jsCallDetail=True, nativeDetail=True, wasm=True, objectSnapshots=True,
                           objectDepth=2, objectNodes=8, objectProperties=32, detailScriptInclude='fixture.js',
                           detailExcludeInternal=True)
        if opcode:
            options['opcode'] = True
        file = self.root / 'capture-options.json'
        write_json(file, options)
        try:
            try:
                invoke('capture', '--config', file, '--assets', assets, timeout=150)
                summary = json.loads((self.root / 'capture/summary.json').read_bytes())
            except AssertionError:
                capture = self.root / 'capture'
                evidence = {}
                for name in ('browser.json', 'cdp-event-error.json', 'summary.json'):
                    if (capture / name).exists():
                        evidence[name] = json.loads((capture / name).read_bytes())
                if (capture / 'cdp.jsonl').exists():
                    evidence['cdp-tail'] = [json.loads(line) for line in (capture / 'cdp.jsonl').read_bytes().splitlines()[-16:]]
                REPORT.append(dict(test=self.id(), conclusion='failed', diagnostics=evidence))
                raise
            capture = self.root / 'capture'
            resources = [json.loads(line) for line in (capture / 'catalog/resources.jsonl').read_bytes().splitlines()]
            matches = [row for row in resources if row.get('request', {}).get('url', '').endswith('/body') and row.get('body')]
            self.assertTrue(matches, summary)
            self.assertTrue(any((capture / row['body']['path']).read_bytes() == body for row in matches))
            console = (capture / 'events/console.jsonl').read_text(encoding='utf-8')
            self.assertIn('CPP_BROWSER_DONE', console)
            self.assertGreater(summary['counts']['script'], 0)
            self.assertGreater(summary['counts']['env'], 0)
            if mode == 'native':
                self.assertGreater(summary['counts']['jscall'], 0)
                self.assertGreater(summary['counts']['native'], 0)
                self.assertGreater(summary['counts']['wasm'], 0)
                records = [json.loads(line) for line in (capture / 'events/jscall.jsonl').read_bytes().splitlines()]
                self.assertTrue(any(marker == arg.get('value') for row in records for arg in row['payload'].get('arguments', [])))
            if opcode:
                self.assertGreater(summary['counts']['opcode'], 0)
                records = [json.loads(line) for line in (capture / 'events/opcode.jsonl').read_bytes().splitlines()]
                self.assertTrue(any(row['payload']['engine'] == 'Ignition' and row['payload']['bytes'] for row in records))
            REPORT.append(dict(test=self.id(), conclusion='passed', mode=mode, counts=summary['counts'], integrity=summary['integrityConclusion'], gaps=summary['gaps']))
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    @unittest.skipUnless(ARGS.chrome, 'requires explicit --chrome')
    def test_real_chrome_native(self):
        self.chrome_capture('native')

    @unittest.skipUnless(ARGS.chrome, 'requires explicit --chrome')
    def test_real_chrome_opcode(self):
        self.chrome_capture('native', opcode=True)

    @unittest.skipUnless(ARGS.chrome, 'requires explicit --chrome')
    def test_real_chrome_cdp(self):
        self.chrome_capture('cdp')


if __name__ == '__main__':
    program = unittest.main(argv=[__file__, *REMAINING], exit=False, verbosity=2)
    if ARGS.evidence:
        file = Path(ARGS.evidence)
        file.parent.mkdir(parents=True, exist_ok=True)
        write_json(file, dict(schemaVersion=1, passed=program.result.wasSuccessful(), testsRun=program.result.testsRun,
                             failures=len(program.result.failures), errors=len(program.result.errors), skipped=len(program.result.skipped), records=REPORT,
                             temporaryFixturesRemoved=True))
    raise SystemExit(0 if program.result.wasSuccessful() else 1)
