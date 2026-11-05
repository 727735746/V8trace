import datetime
import hashlib
import importlib.util
import json
import mmap
from pathlib import Path, PurePosixPath


spec = importlib.util.spec_from_file_location('native_media_build', Path(__file__).with_name('media_build.py'))
media_build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media_build)


def digest(file):
    value = hashlib.sha256()
    with file.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1048576), b''):
            value.update(chunk)
    return value.hexdigest()


def load(file):
    return json.loads(file.read_text('utf-8-sig'))


def write(file, value):
    file.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def write_metadata(output, projection_file, receipt_file, dependencies_file):
    projection, receipt = load(projection_file), load(receipt_file)
    if (projection.get('edition') != 'community' or projection.get('professionalProducersIncluded') is not False
            or receipt['status'] != 'compiled-not-validated' or receipt['exitCode'] != 0
            or receipt['releaseDefaults']['edition'] != 'community'
            or receipt['releaseDefaults']['sourceProjectionSha256'] != digest(projection_file)):
        raise RuntimeError('A source-bound successful Community build is required')
    compiled = {row['path']: row['sha256'] for row in receipt['sources']}
    if not projection.get('files'):
        raise RuntimeError('Community source manifest is empty')
    for row in projection['files']:
        if compiled.get(row['path']) != row['sha256']:
            raise RuntimeError('Compiled shared source differs: ' + row['path'])
    args_hash = receipt['releaseDefaults'].get('gnArgsSha256')
    if receipt['releaseDefaults'].get('fieldTrialTestingConfig') is not False or args_hash != digest(output / 'args.gn'):
        raise RuntimeError('Compiled GN arguments changed')
    media = media_build.verify(output / 'args.gn')
    if receipt['releaseDefaults'].get('mediaBuild') != media:
        raise RuntimeError('Compiled media configuration differs')
    core = {row['path']: row['sha256'] for row in receipt['binaries']}
    if set(core) != {'chrome.exe', 'chrome.dll', 'chrome_elf.dll', 'v8_context_snapshot.bin'}:
        raise RuntimeError('Incomplete native binary evidence')
    for name, expected in core.items():
        if digest(output / name) != expected:
            raise RuntimeError('Compiled browser changed: ' + name)
    forbidden = ['V8TraceOpcode', 'V8TraceWasm', 'V8TraceCausality', 'V8TraceCausalEvent', '"vm-state"']
    with (output / 'chrome.dll').open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as binary:
        if binary.find(b'v8trace/browser-edition/1') < 0:
            raise RuntimeError('Compiled browser has no Community producer identity')
        for token in forbidden:
            if binary.find(token.encode()) >= 0 or binary.find(token.encode('utf-16-le')) >= 0:
                raise RuntimeError('Professional producer remains in Community binary: ' + token)
    root_hash, v8_hash = projection['patchHashes']['chromium'], projection['patchHashes']['v8']
    for role, expected in projection['patchHashes'].items():
        if digest(projection_file.parent / ('community-' + role + '.patch')) != expected:
            raise RuntimeError('Projected public patch changed')
    dependencies = load(dependencies_file)['//chrome:chrome']['runtime_deps']
    files = set(core)
    for entry in dependencies:
        relative = PurePosixPath(entry.replace('\\', '/'))
        if relative.suffix.lower() == '.pdb' or relative.parts[0] == 'initialexe':
            continue
        if relative.is_absolute() or '..' in relative.parts or ':' in str(relative):
            raise RuntimeError('GN runtime dependency leaves output: ' + entry)
        file = output.joinpath(*relative.parts)
        if not file.resolve(strict=True).is_relative_to(output) or not file.is_file():
            raise RuntimeError('Invalid GN runtime dependency: ' + entry)
        for path in [file, *file.parents]:
            if path == output:
                break
            if path.lstat().st_file_attributes & 1024:
                raise RuntimeError('Reparse point in runtime dependency: ' + entry)
        files.add(relative.as_posix())
    if not 5 <= len(files) <= 100000:
        raise RuntimeError('Invalid Community runtime inventory size')
    descriptor = dict(schemaVersion=1, backend='chromium-opcode', protocol='v8trace/opcode/1',
        category='disabled-by-default-v8.opcode', edition='community', editionProtocol='v8trace/edition/1',
        producerEditionProtocol='v8trace/browser-edition/1', chromiumRevision=projection['chromiumRevision'],
        v8Revision=projection['v8Revision'], patchSha256=v8_hash, sourcePatchHashes=projection['patchHashes'],
        executableSha256=core['chrome.exe'], binaryHashes=core,
        builtAt=datetime.datetime.now(datetime.timezone.utc).isoformat(), opcodeSupported=False,
        professionalProducersIncluded=False, nativeRunValidated=False,
        buildEvidenceProtocol='v8trace/build-supervisor/1', buildAttemptId=receipt['attemptId'],
        buildReceiptSha256=digest(receipt_file), sourceProjectionSha256=digest(projection_file),
        buildResourceProfile=receipt['releaseDefaults'])
    descriptor['mediaBuild'] = media
    protocols = dict(startup='lpac-startup/1', imeCompat='ime-compat/1', language='languages/1',
        startupDefaults='startup-defaults/1', detail='detail/1', objectSnapshot='objects/1',
        detailFilter='detail-filter/1', detailHotPath='detail-hotpath/1', call='call/2',
        functionDictionary='function/1', callStats='call-stats/1', callValueFilter='detail-filter/2',
        envProbe='env/1', envStats='env-stats/1', envBuiltin='env-builtin/1', closure='closure/1',
        state='state/1', stateStats='state-stats/1', identity='identity/1', aux='aux/1',
        bytecodeIdentity='bytecode-identity/1', browser='browser/1', redirectBody='redirect-body/1',
        recordingControl='recording/1', traceTransport='trace-transport/1')
    descriptor.update({key + 'Protocol': 'v8trace/' + value for key, value in protocols.items()})
    shared_native = Path(__file__).resolve().parents[3] / 'native'
    drain_headers = ('v8trace-recording-control.h', 'v8trace-recording-browser.h')
    if all((shared_native / name).is_file() and compiled.get('chrome/browser/' + name) == digest(shared_native / name) for name in drain_headers):
        descriptor['httpDrainProtocol'] = 'v8trace/http-drain/1'
    trace_sources = ['content/browser/tracing/tracing_controller_impl.cc', 'content/browser/tracing/tracing_controller_impl.h',
                     'content/browser/tracing/tracing_controller_impl_data_endpoint.cc', 'content/public/browser/tracing_controller.h',
                     'services/tracing/perfetto/consumer_host.cc',
                     'third_party/perfetto/src/tracing/service/tracing_service_impl.cc',
                     'third_party/perfetto/src/tracing/service/tracing_service_session.h',
                     'services/tracing/public/mojom/perfetto_service.mojom',
                     'services/tracing/public/mojom/trace_config_mojom_traits.h',
                     'services/tracing/public/mojom/trace_config_mojom_traits.cc']
    projected_trace = [projection_file.parent / 'apply-check' / name for name in trace_sources]
    if ((shared_native / 'v8trace-recording-control.h').is_file()
            and compiled.get('chrome/browser/v8trace-recording-control.h') == digest(shared_native / 'v8trace-recording-control.h')
            and all(file.is_file() and compiled.get(name) == digest(file) for name,file in zip(trace_sources,projected_trace))
            and 'v8-trace-protobuf-export' in projected_trace[0].read_text('utf-8')
            and 'v8trace_protobuf_stream_' in projected_trace[1].read_text('utf-8')
            and all(marker in projected_trace[2].read_text('utf-8') for marker in
                    ('"v8-trace-protobuf-export"', '? base::cstring_view("wb")', ': base::cstring_view("w")'))
            and all('base::OpenFile(' + name + ', file_mode)' in projected_trace[2].read_text('utf-8')
                    for name in ('pending_file_path_', 'file_path_'))
            and all(marker in projected_trace[0].read_text('utf-8') for marker in
                    ('set_write_into_file(true)', 'set_file_write_period_ms(1000)', 'set_max_file_size_bytes(max_file_bytes)',
                     'StopTracingToFile', 'base::UnguessableToken::Create().ToString()'))
            and all(marker in projected_trace[3].read_text('utf-8') for marker in ('StartTracingToFile', 'StopTracingToFile'))
            and all(marker in projected_trace[4].read_text('utf-8') for marker in ('_open_osfhandle', '_O_WRONLY | _O_BINARY', 'output_file.TakePlatformFile()'))
            and all(marker in projected_trace[5].read_text('utf-8') for marker in
                    ('v8trace/streaming-file/2/', 'v8trace_file_write_failed', 'v8trace_file_size_limit_reached', 'expected_wr_size',
                     'if (tracing_session->config.unique_session_name().rfind(\n'
                     '            "v8trace/streaming-file/2/", 0) == 0) {\n'
                     '      tracing_session->should_emit_stats = true;\n'
                     '    }\n'
                     '    tracing_session->write_period_ms = 0;'))
            and all(marker in projected_trace[6].read_text('utf-8') for marker in ('v8trace_file_write_failed', 'v8trace_file_size_limit_reached'))
            and all(marker in projected_trace[7].read_text('utf-8') for marker in
                    ('uint32 file_write_period_ms;', 'uint64 max_file_size_bytes;', 'string? v8trace_file_session_name;'))
            and all(marker in projected_trace[8].read_text('utf-8') for marker in
                    ('src.file_write_period_ms()', 'src.max_file_size_bytes()', 'v8trace_file_session_name', 'v8trace/streaming-file/2/'))
            and all(marker in projected_trace[9].read_text('utf-8') for marker in
                    ('ReadV8traceFileSessionName', 'out->set_file_write_period_ms(data.file_write_period_ms())',
                     'out->set_max_file_size_bytes(data.max_file_size_bytes())', 'DeserializeFromString',
                     'name.substr(sizeof(prefix) - 1)', 'name.size() > 128', 'data.file_write_period_ms() != 1000',
                     '!data.max_file_size_bytes()', '!data.write_into_file()'))):
        descriptor['nativeTraceExportProtocol'] = 'v8trace/perfetto-file-export/2'
        descriptor['traceConfigIpcProtocol'] = 'v8trace/perfetto-file-config-ipc/1'
    producer_sources = ['services/tracing/public/cpp/tracing_features.h',
                        'services/tracing/public/cpp/tracing_features.cc',
                        'services/tracing/public/cpp/perfetto/perfetto_traced_process.cc']
    projected_producer = [projection_file.parent / 'apply-check' / name for name in producer_sources]
    if (all(file.is_file() and compiled.get(name) == digest(file) for name,file in zip(producer_sources,projected_producer))
            and 'BASE_DECLARE_FEATURE_PARAM(int, kV8TraceBatchCommitsDurationMs);' in projected_producer[0].read_text('utf-8')
            and all(marker in projected_producer[1].read_text('utf-8') for marker in
                    ('kV8TraceBatchCommitsDurationMs,', '&kPerfettoBackendParams,',
                     '"v8trace_batch_commits_ms",\n                   1000);'))
            and all(marker in projected_producer[2].read_text('utf-8') for marker in
                    ('features::kV8TraceBatchCommitsDurationMs.Get();',
                     'v8trace_batch_commits_ms >= 0 && v8trace_batch_commits_ms <= 1000',
                     '? static_cast<uint32_t>(v8trace_batch_commits_ms)\n          : 1000;'))):
        descriptor['traceProducerCommitProtocol'] = 'v8trace/perfetto-producer-commit/1'
    timeout_sources = ['services/tracing/public/mojom/perfetto_service.mojom',
                       'services/tracing/public/mojom/trace_config_mojom_traits.h',
                       'services/tracing/public/mojom/trace_config_mojom_traits.cc',
                       'services/tracing/public/cpp/perfetto/perfetto_config.cc']
    projected_timeout = [projection_file.parent / 'apply-check' / name for name in timeout_sources]
    if (all(file.is_file() and compiled.get(name) == digest(file) for name,file in zip(timeout_sources,projected_timeout))
            and all(marker in projected_timeout[0].read_text('utf-8') for marker in
                    ('uint32 flush_timeout_ms;', 'uint32 data_source_stop_timeout_ms;'))
            and all(marker in projected_timeout[1].read_text('utf-8') for marker in
                    ('return src.flush_timeout_ms();', 'return src.data_source_stop_timeout_ms();'))
            and all(marker in projected_timeout[2].read_text('utf-8') for marker in
                    ('out->set_flush_timeout_ms(data.flush_timeout_ms());',
                     'out->set_data_source_stop_timeout_ms(data.data_source_stop_timeout_ms());',
                     'data.flush_timeout_ms() < 100', 'data.flush_timeout_ms() > 3600000',
                     'data.data_source_stop_timeout_ms() < 100', 'data.data_source_stop_timeout_ms() > 3600000'))
            and all(marker in projected_timeout[3].read_text('utf-8') for marker in
                    ('"v8-trace-flush-timeout-ms"', 'timeout_ms >= 100 && timeout_ms <= 3600000',
                     'perfetto_config.set_flush_timeout_ms(timeout_ms);',
                     'perfetto_config.set_data_source_stop_timeout_ms(timeout_ms);'))):
        descriptor['traceConfigTimeoutIpcProtocol'] = 'v8trace/perfetto-flush-timeout-ipc/1'
    file_io_sources = ['third_party/perfetto/src/tracing/service/tracing_service_impl.cc',
                       'third_party/perfetto/src/tracing/service/tracing_service_impl.h',
                       'third_party/perfetto/src/tracing/service/v8trace-file-writer.h']
    projected_file_io = [projection_file.parent / 'apply-check' / name for name in file_io_sources]
    file_io_header = shared_native / 'perfetto-file-writer.h'
    if (file_io_header.is_file()
            and all(file.is_file() and compiled.get(name) == digest(file) for name,file in zip(file_io_sources,projected_file_io))
            and digest(projected_file_io[2]) == digest(file_io_header)
            and 'bool read_continuation = false' in projected_file_io[1].read_text('utf-8')
            and all(marker in projected_file_io[0].read_text('utf-8') for marker in
                    ('v8trace_file_io::WriteCoalesced(', 'v8trace_complete_write = result.complete;',
                     'if (wr_size <= 0 || !v8trace_complete_write ||',
                     'if (read_continuation &&', 'tracing_session->write_period_ms == 0',
                     'if (!tracing_session || !tracing_session->write_into_file)',
                     'v8trace_file && async_flush_buffers_before_read && has_more',
                     'tracing_session->state == TracingSession::STARTED &&',
                     'tracing_session->write_period_ms != 0', '/*read_continuation=*/true',
                     'if (async_flush_buffers_before_read && !read_continuation)',
                     'do_read_buffers_into_file_fn(async_flush_buffers_before_read);'))):
        descriptor['nativeFileIoProtocol'] = 'v8trace/perfetto-file-io/1'
    script_names = ('v8trace-browser.h', 'v8trace-script-intern-cache.h',
                    'v8trace-sha256.h', 'v8trace-wire-string-table.h')
    script_sources = ['v8/src/runtime/' + name for name in script_names]
    projected_scripts = [projection_file.parent / 'apply-check' / name for name in script_sources]
    shared_scripts = [shared_native / name for name in script_names]
    if (all(file.is_file() and shared.is_file() and digest(file) == digest(shared)
            and compiled.get(name) == digest(file) for name,file,shared in zip(script_sources,projected_scripts,shared_scripts))
            and all(marker in projected_scripts[0].read_text('utf-8') for marker in
                    ('v8trace/native-script-interning/1', 'JsonString(encoded, source);',
                     'v8trace_wire::StringInternedData::Get(', 'std::make_pair(digest, literal.size())',
                     'annotation->set_string_value_iid(iid);', 'nativeSourceJsonSha256'))
            and all(marker in projected_scripts[1].read_text('utf-8') for marker in
                    ('kCapacity = 256', 'std::min_element(values_.begin(), values_.end()',
                     'return left.second.last_used < right.second.last_used;', '*iid = ++next_;'))
            and all(marker in projected_scripts[3].read_text('utf-8') for marker in
                    ('namespace v8trace_wire', 'struct StringInternedData',
                     'kDebugAnnotationStringValuesFieldNumber', 'v8trace_script_intern::CacheTraits',
                     'reinterpret_cast<const uint8_t*>(source.data())',
                     'InternedDebugAnnotationName::Get(&context, name)',
                     'annotation->set_string_value_iid(iid);'))):
        descriptor['nativeScriptInterningProtocol'] = 'v8trace/native-script-interning/1'
    wire_header = shared_native / 'network-http.h'
    wire_loader = projection_file.parent / 'apply-check/services/network/url_loader.cc'
    if (wire_header.is_file() and compiled.get('services/network/v8trace-http.h') == digest(wire_header)
            and wire_loader.is_file() and compiled.get('services/network/url_loader.cc') == digest(wire_loader)
            and wire_loader.read_text('utf-8').count('v8trace_http::WireRequestHeaders(url_request_.get(), headers);') == 1):
        descriptor['httpWireHeadersProtocol'] = 'v8trace/http-wire-headers/1'
    for key in ['detailFilter', 'envProbe', 'closure', 'identity', 'aux']:
        descriptor[key + 'PatchSha256'] = v8_hash
    for key in ['state', 'browser']:
        descriptor[key + 'PatchSha256'] = root_hash
    for key in ['startup', 'imeCompat', 'language', 'objectSwitch', 'recordingControl']:
        descriptor[key + 'PatchSha256'] = root_hash
    descriptor.update(detailFeatures=['jscall', 'native'], envProbeFeatures=['absent', 'interfaces', 'mutations', 'inventory'],
        stateFeatures=['websocket', 'indexeddb', 'cookie', 'storage', 'wasm-module'],
        browserFeatures=['env', 'http', 'script', 'console', 'exception'],
        preferredLanguages=['en-US', 'en'], applicationLocale='en-US', fieldTrialTestingConfig=False,
        gnArgsSha256=digest(output / 'args.gn'), apiKeyInfoBarSuppressed=True,
        imeCompatScope='tsf-native-calls;thread-policy-restored;process-policy-preserved',
        recordingControlTransport='local-files-no-cdp', traceSharedMemoryMaxBytes=8388608)
    descriptor_file = output / 'v8trace-opcode.json'
    inventory_file = output / 'v8trace-runtime-files.json'
    if descriptor_file.exists() or inventory_file.exists():
        raise RuntimeError('Browser metadata already exists; keep the prior evidence')
    write(descriptor_file, descriptor)
    files.update(['v8trace-opcode.json', 'v8trace-runtime-files.json'])
    write(inventory_file, dict(schemaVersion=1, protocol='v8trace/runtime-files/1', source='GN runtime_deps',
        descriptorSha256=digest(descriptor_file), files=sorted(files)))
    result = dict(protocol='v8trace/community-browser-package/1', status='compiled-awaiting-native-validation',
        edition='community', executable=str(output / 'chrome.exe'), files=len(files), binaryHashes=core,
        descriptorSha256=digest(descriptor_file), inventorySha256=digest(inventory_file),
        privateProducerBinaryScan='PASS', sourceProjectionSha256=digest(projection_file),
        nativeRunValidated=False)
    return result
