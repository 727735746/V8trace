import argparse
import base64
import hashlib
import http.client
import json
import os
import pathlib
import queue
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import uuid


CHECKS = 0


def physical_path(value):
    value = pathlib.Path(value).absolute()
    if os.name != "nt":
        return value
    name = str(value)
    if name.startswith("\\\\?\\"):
        return value
    return pathlib.Path("\\\\?\\UNC\\" + name[2:] if name.startswith("\\\\") else "\\\\?\\" + name)


def index_path(root, name, extended):
    value = root / name
    if extended:
        while len(str(value).encode("utf-16-le")) // 2 < 420:
            value /= "中文 深层索引缓存保留原路径-0123456789"
    return value
META = {
    "io.modelcontextprotocol/protocolVersion": "2026-07-28",
    "io.modelcontextprotocol/clientInfo": {"name": "v8trace-test", "version": "1"},
    "io.modelcontextprotocol/clientCapabilities": {},
}


def check(value, message):
    global CHECKS
    CHECKS += 1
    if not value:
        raise AssertionError(message)


def modern(method, params=None, request_id=1):
    return {"jsonrpc": "2.0", "id": request_id, "method": method,
            "params": dict(params or {}, _meta=META)}


def data(reply):
    check("result" in reply, f"Expected result: {reply}")
    check(not reply["result"].get("isError"), f"Tool failed: {reply}")
    return reply["result"]["structuredContent"]


class Stdio:
    def __init__(self, command, log, timeout=20):
        self.timeout = timeout
        self.log = log.open("wb")
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, bufsize=0)
        self.replies = queue.Queue()
        self.ident = 0
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        try:
            for line in self.process.stdout:
                self.replies.put(json.loads(line))
        except Exception as error:
            self.replies.put(error)

    def send(self, message):
        wire = message if isinstance(message, bytes) else json.dumps(message, ensure_ascii=False).encode("utf-8")
        self.process.stdin.write(wire + b"\n")
        self.process.stdin.flush()

    def receive(self):
        value = self.replies.get(timeout=self.timeout)
        if isinstance(value, Exception):
            raise value
        return value

    def rpc(self, method, params=None, newer=False):
        self.ident += 1
        message = modern(method, params, self.ident) if newer else {
            "jsonrpc": "2.0", "id": self.ident, "method": method, "params": params or {}}
        self.send(message)
        result = self.receive()
        check(result.get("id") == self.ident, "Stdio response ID")
        return result

    def call(self, name, arguments=None, newer=False):
        return self.rpc("tools/call", {"name": name, "arguments": arguments or {}}, newer)

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.reader.join(timeout=2)
        self.log.close()
        check(self.process.returncode == 0, "Stdio graceful exit")


def wait_index(call):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        status = data(call("get_file_status"))
        if status["state"] == "ready":
            return status
        check(status["state"] == "indexing", f"Index state: {status}")
        time.sleep(0.01)
    raise AssertionError("Index deadline")


def causal_fixture(source):
    capture = source / "causal-capture"
    (capture / "events").mkdir(parents=True)
    (capture / "blobs/sha256").mkdir(parents=True)
    producer = "0123456789abcdef0123456789abcdef"
    rows = []

    def emit(kind, task="0", parent="0", **extra):
        payload = dict(protocol="v8trace/causality/1", producerId=producer, epoch="1",
                       sequence=str(len(rows) + 1), taskId=task, operationId=task,
                       resourceId="0", relatedTaskId=parent, taskKind="invocation",
                       kind=kind, pid=1, tid="2")
        payload.update(extra)
        rows.append(dict(category="causality", payload=payload))

    emit("session-start", reason="v8trace/causality-model/2")
    emit("registered", "1")
    emit("invocation-start", "1")
    emit("registered", "2", "1")
    emit("invocation-start", "2")
    emit("http-root", "2", layer="resource", rootTaskId="2", identifier="7",
         url="https://fixture.test/完整payload", method="POST",
         callIdentity=dict(protocol="v8trace/call-position/1", producerId="42",
                           jsEventSequence="3", activeJSCallKnown=True))
    emit("invocation-end", "2")
    emit("local-subtree-closed", "2")
    emit("invocation-end", "1")
    emit("local-subtree-closed", "1")
    emit("session-cutoff")
    rows.append(dict(category="causality", payload=dict(
        protocol="v8trace/causality/1", producerId=producer, epoch="1",
        kind="producer-final", lastSequence=str(len(rows)), attempted=str(len(rows)),
        emitted=str(len(rows)), dropped="0", pendingEmissions="0", violations="0",
        activeNodesAtCutoff="0")))
    (capture / "events/causality.jsonl").write_text(
        "".join(json.dumps(row, ensure_ascii=False) + "\n" for row in rows), encoding="utf-8")
    calls = []
    for sequence, task, phase, call_id, function in [
            (1, "1", "entry", "10", "trigger"),
            (2, "2", "entry", "11", "encryptPayload"),
            (3, "2", "return", "11", "encryptPayload"),
            (4, "2", "entry", "12", "afterRequest")]:
        calls.append(dict(category="jscall", kind="call-" + phase, seq=sequence,
                          processId=1, threadId=2, payload=dict(
                              producerId="42", producerSeq=sequence, callId=call_id,
                              phase=phase, functionName=function, scriptId=7,
                              scriptUrl="https://fixture.test/计算.js", valuesCaptured=True,
                              arguments=["完整参数\x00payload"], causal=dict(
                                  producerId=producer, epoch="1", taskId=task, known=True))))
    (capture / "events/jscall.jsonl").write_text(
        "".join(json.dumps(row, ensure_ascii=False) + "\n" for row in calls), encoding="utf-8")
    body = bytes(range(256)) * 400
    digest = hashlib.sha256(body).hexdigest()
    (capture / "blobs/sha256" / digest).write_bytes(body)
    reference = dict(path="blobs/sha256/" + digest, sha256=digest, bytes=len(body),
                     representation="native-http-upload-bytes")
    request = dict(category="http", kind="request-complete", processId=1,
                   payload=dict(resourceIdentifier="7", nativeLayer="resource",
                                requestId="1/resource:7:1", postBody=reference))
    (capture / "events/http.jsonl").write_text(json.dumps(request) + "\n", encoding="utf-8")
    (capture / "capture-options.json").write_text(json.dumps(dict(
        schemaVersion=1, protocol="v8trace/capture-options/1",
        options=dict(httpRootTrace=True, http=True, jsCallDetail=True))), encoding="utf-8")
    (capture / "summary.json").write_text(json.dumps(dict(
        captureConclusion="collected", integrityConclusion="complete", criticalGapCount=0)), encoding="utf-8")
    (capture / "gaps.jsonl").write_bytes(b"")


def causal_tools_test(call):
    data(call("v8trace_open_capture", {"capture": "causal-capture"}))
    roots = data(call("v8trace_causal_roots", {"limit": 1}))
    check(len(roots["rows"]) == 1, "Actual MCP native root enumeration")
    options = dict(rootId=roots["rows"][0]["rootId"], generation=roots["generation"])
    root = data(call("v8trace_causal_root", options))
    check(root["root"]["rootId"] == options["rootId"], "Opaque root identity round trip")
    nodes = data(call("v8trace_causal_nodes", dict(options, direction="upstream")))
    check(len(nodes["rows"]) == 2, "Exact trigger and sender ancestry through MCP")
    points = data(call("v8trace_causal_checkpoints", dict(options, kindPrefix="http-")))
    check(any(row.get("kind") == "http-root" for row in points["rows"]), "HTTP checkpoint original evidence")
    functions = []
    query = dict(options, limit=1, channel="jscall")
    cursor = None
    for _ in range(20):
        page = data(call("v8trace_request_functions", query))
        functions.extend(page["rows"])
        if not page["hasMore"]:
            break
        cursor = page["nextCursor"]
        query["cursor"] = cursor
    check(len(functions) == 3, "Every prior function activation retained across actual MCP pages")
    check(all(row["requestPosition"] == "observed-before-request" for row in functions), "Request counter proof survives MCP")
    row = functions[1]
    raw = data(call("v8trace_read_evidence", dict(rowId=row["rowId"], generation=row["generation"])))
    check(json.loads(base64.b64decode(raw["base64"]))["payload"]["arguments"] == ["完整参数\x00payload"], "Exact original call values over MCP")
    check(call("v8trace_request_functions", dict(options, cursor=cursor, direction="downstream", limit=1))["result"]["isError"], "MCP cursor binds selection and direction")
    check(call("v8trace_causal_root", dict(options, generation="stale"))["result"]["isError"], "MCP root generation rejects stale graph")
    after = data(call("v8trace_request_functions", dict(options, includeAfterRequest=True)))
    check(len(after["rows"]) == 4, "Explicit post-request call selection")
    http = data(call("v8trace_request_functions", dict(options, channel="http")))
    check(len(http["rows"]) == 1, "Exact native resource to HTTP payload binding over MCP")
    reference = http["rows"][0]["blobRefs"][0]
    body = bytearray()
    offset = 0
    while True:
        part = data(call("v8trace_read_evidence", dict(reference=reference, offset=offset, limit=65536)))
        body.extend(base64.b64decode(part["base64"]))
        if not part["hasMore"]:
            break
        offset = part["nextOffset"]
    check(bytes(body) == bytes(range(256)) * 400, "Full multi-page binary payload over MCP without UTF-8 conversion")
    data(call("v8trace_close_capture"))


def stdio_test(command, root, source, index):
    client = Stdio(command + ["--transport", "stdio", "--index-dir", str(index),
                             "--export-root", str(root / "stdio-exports")], root / "stdio.log")
    try:
        discovered = client.rpc("server/discover", newer=True)["result"]
        check(discovered["resultType"] == "complete" and "2026-07-28" in discovered["supportedVersions"], "Modern discover without handshake")
        init = client.rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                         "clientInfo": {"name": "test", "version": "1"}})
        check(init["result"]["protocolVersion"] == "2025-11-25", "Legacy initialize")
        client.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        listing = client.rpc("tools/list")["result"]["tools"]
        names = {tool["name"] for tool in listing}
        basic = {"open_file", "get_file_status", "close_file", "read_lines", "search_lines", "export_lines", "v8trace_open_capture", "v8trace_capture_status", "v8trace_close_capture", "v8trace_query", "v8trace_read_evidence"}
        advanced = {"v8trace_causal_roots", "v8trace_causal_root", "v8trace_causal_nodes", "v8trace_causal_checkpoints", "v8trace_request_functions"}
        professional = "v8trace_causal_roots" in names
        check(names == basic | (advanced if professional else set()), "Exact compiled edition tool schemas")
        check(all(tool["inputSchema"]["type"] == "object" for tool in listing), "Object schemas")
        data(client.call("open_file", {"path": "lines.log"}))
        check(wait_index(client.call)["line_count"] == 3, "Stdio line count")
        read = data(client.call("read_lines", {"start_line": 1, "line_count": 2}, newer=True))
        check(read["content"] == "alpha\r\nbeta\ufffd\n" and read["lossy_utf8"], "UTF-8 loss and raw line endings")
        hits = data(client.call("search_lines", {"pattern": "a", "start_line": 1,
                                                  "max_scan_lines": 2**64 - 1, "max_matches": 1}))
        check(hits["next_line"] == 2 and hits["match_limit_reached"], "Uint64 scan range and cursor")
        export = data(client.call("export_lines", {"start_line": 1, "line_count": 2, "output_path": "exact.log"}))
        check(pathlib.Path(export["output_path"]).read_bytes() == b"alpha\r\nbeta\xff\n", "MCP raw export")
        check(client.call("export_lines", {"start_line": 1, "line_count": 1, "output_path": "../escape.log"})["result"]["isError"], "MCP export confinement")
        check(client.call("open_file", {"path": "../outside.log"})["result"]["isError"], "MCP source confinement")
        invalid = client.call("read_lines", {"start_line": -1, "line_count": 1})
        check(invalid["error"]["code"] == -32602, "Schema validation")
        check(client.call("missing")["error"]["code"] == -32602, "Unknown tool")
        client.send(b"{")
        check(client.receive()["error"]["code"] == -32700, "Parse error")
        client.send(b"[]")
        check(client.receive()["error"]["code"] == -32600, "Batch rejected")
        client.send(b"x" * (1048576 + 1))
        check(client.receive()["error"]["code"] == -32600, "Bounded stdio input")
        capture = data(client.call("v8trace_open_capture", {"capture": "capture"}))
        check(capture["records"] >= 3 and not capture["completeCausality"], "Structured index boundary")
        check(pathlib.Path(capture["indexOwner"]) == index, "Structured cache uses selected ordinary index directory")
        check(any(physical_path(index).glob("analysis-cpp-*/index.sqlite")), "Actual structured cache below selected root")
        rows = data(client.call("v8trace_query", {"category": "jscall", "script": "https://fixture.test/计算.js", "limit": 1}))
        check(len(rows["rows"]) == 1 and rows["hasMore"], "Exact metadata pagination")
        row = rows["rows"][0]
        exact = data(client.call("v8trace_read_evidence", {"rowId": row["rowId"], "generation": row["generation"], "offset": 0, "limit": 65536}))
        raw = source.joinpath("capture/events/jscall.jsonl").read_bytes().splitlines()[0]
        check(base64.b64decode(exact["base64"]) == raw, "Original structured row exact bytes")
        check(row["sourceFileSha256"] == hashlib.sha256(source.joinpath("capture/events/jscall.jsonl").read_bytes()).hexdigest(), "Structured source hash")
        check(client.call("v8trace_read_evidence", {"rowId": row["rowId"], "generation": "stale"})["result"]["isError"], "Stale generation rejected")
        blob = (source / "capture/blob.json").read_text("utf-8")
        blob_read = data(client.call("v8trace_read_evidence", {"reference": json.loads(blob)}))
        check(base64.b64decode(blob_read["base64"]) == b"\x00\xffbinary", "Blob hash and exact bytes")
        data(client.call("v8trace_close_capture"))
        if professional:
            causal_tools_test(client.call)
        else:
            check("error" in client.call("v8trace_causal_roots", {}), "Community rejects unadvertised professional MCP tool")
        data(client.call("close_file"))
        data(client.call("open_file", {"path": "long.log"}))
        wait_index(client.call)
        request_id = "cancel-and-duplicate"
        client.send({"jsonrpc": "2.0", "id": request_id, "method": "tools/call", "params": {
            "name": "search_lines", "arguments": {"pattern": "(x+x+)+y", "regex": True,
                "start_line": 1, "max_scan_lines": 2**64 - 1}}})
        client.send({"jsonrpc": "2.0", "id": request_id, "method": "ping"})
        client.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": request_id}})
        answers = [client.receive(), client.receive()]
        check(any(answer.get("error", {}).get("code") == -32600 for answer in answers), "Duplicate in-flight IDs")
        check(any(answer.get("result", {}).get("structuredContent", {}).get("cancelled") for answer in answers), "Stdio query cancellation")
        data(client.call("close_file"))
    finally:
        client.close()


def http_test(command, root, index):
    with socket.socket() as available:
        available.bind(("127.0.0.1", 0))
        port = available.getsockname()[1]
    token = uuid.uuid4().hex + uuid.uuid4().hex
    token_file = root / "token.txt"
    token_file.write_text(token, encoding="utf-8")
    log = (root / "http.log").open("wb")
    process = subprocess.Popen(command + ["--transport", "http", "--port", str(port),
        "--token-file", str(token_file), "--index-dir", str(index),
        "--export-root", str(root / "http-exports"), "--query-concurrency", "1"],
        stdin=subprocess.DEVNULL, stdout=log, stderr=log)
    ident = 0

    def wire(message=None, path="/mcp", method="POST", override=None):
        headers = {"Authorization": "Bearer " + token, "Accept": "application/json, text/event-stream",
                   "Content-Type": "application/json", "MCP-Protocol-Version": "2025-11-25"}
        if message and "_meta" in message.get("params", {}):
            headers["MCP-Protocol-Version"] = message["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"]
            headers["Mcp-Method"] = message["method"]
            if message["method"] == "tools/call":
                headers["Mcp-Name"] = message["params"]["name"]
        headers.update(override or {})
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
        body = json.dumps(message, ensure_ascii=False).encode("utf-8") if message is not None else b""
        connection.request(method, path, body=body, headers=headers)
        reply = connection.getresponse()
        status, content_type = reply.status, reply.getheader("Content-Type", "")
        raw = reply.read()
        connection.close()
        if content_type.startswith("text/event-stream"):
            events = [json.loads(line[6:]) for line in raw.decode("utf-8").splitlines() if line.startswith("data: ")]
            check(len(events) == 1, "SSE exactly one final JSON-RPC response")
            return status, events[0]
        return status, json.loads(raw) if raw else None

    def rpc(method, params=None, newer=True):
        nonlocal ident
        ident += 1
        message = modern(method, params, ident) if newer else {
            "jsonrpc": "2.0", "id": ident, "method": method, "params": params or {}}
        status, reply = wire(message)
        check(status == 200, f"HTTP RPC status: {status}, {reply}")
        return reply

    def call(name, arguments=None):
        return rpc("tools/call", {"name": name, "arguments": arguments or {}})

    try:
        deadline = time.monotonic() + 20
        while True:
            try:
                status, health = wire(path="/healthz", method="GET")
                if status == 200:
                    break
            except OSError:
                pass
            check(process.poll() is None and time.monotonic() < deadline, "HTTP startup deadline")
            time.sleep(0.05)
        professional = health["edition"]["id"] == "professional"
        check(health["runtime"] == "cpp" and health["tools"] == (16 if professional else 11), "Native HTTP edition health")
        check(wire(path="/healthz", method="GET", override={"Authorization": "Bearer bad"})[0] == 401, "HTTP authentication")
        check(wire(path="/healthz", method="GET", override={"Host": "evil.test"})[0] == 403, "Host confinement")
        check(wire(path="/healthz", method="GET", override={"Origin": "https://evil.test"})[0] == 403, "Origin confinement")
        check(wire(path="/mcp", method="GET")[0] == 405, "No standalone GET stream")
        with socket.create_connection(("127.0.0.1", port), timeout=5) as oversized:
            oversized.sendall((f"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Length: 1048577\r\n\r\n").encode())
            check(oversized.recv(4096).startswith(b"HTTP/1.1 413 "), "Reject oversized declared body before upload")
        check(wire(modern("tools/list"), override={"Transfer-Encoding": "chunked"})[0] == 400, "Ambiguous framing rejected")
        check(wire(modern("tools/list"), override={"Accept": "application/json"})[0] == 406, "Transport content negotiation")
        check(rpc("server/discover")["result"]["resultType"] == "complete", "HTTP modern discovery")
        check(rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}}, newer=False)["result"]["protocolVersion"] == "2025-11-25", "HTTP legacy negotiation")
        notification = {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}
        check(wire(notification) == (202, None), "Legacy notification empty 202")
        message = modern("tools/list")
        status, mismatch = wire(message, override={"Mcp-Method": "tools/call"})
        check(status == 400 and mismatch["error"]["code"] == -32020, "Modern mirrored method validation")
        message = modern("tools/call", {"name": "get_file_status", "arguments": {}})
        status, mismatch = wire(message, override={"Mcp-Name": "different"})
        check(status == 400 and mismatch["error"]["code"] == -32020, "Modern mirrored tool name validation")
        encoded = "=?base64?" + base64.b64encode(b"get_file_status").decode() + "?="
        check(wire(message, override={"Mcp-Name": encoded})[0] == 200, "Base64 sentinel header decoding")
        unsupported = modern("tools/list")
        unsupported["params"]["_meta"] = dict(META, **{"io.modelcontextprotocol/protocolVersion": "2099-01-01"})
        status, reply = wire(unsupported)
        check(status == 400 and reply["error"]["code"] == -32022, "Unsupported modern protocol version")
        check(wire(modern("missing"))[0] == 404, "Unknown modern method HTTP 404")
        if professional:
            causal_tools_test(call)
        else:
            status, reply = wire(modern("tools/call", {"name": "v8trace_causal_roots", "arguments": {}}, 9011))
            check(status == 400 and reply.get("error", {}).get("code") == -32602, "Community HTTP rejects professional MCP tool")
        data(call("open_file", {"path": "lines.log"}))
        wait_index(call)
        check(data(call("read_lines", {"start_line": 3, "line_count": 1}))["content"] == "last\n", "HTTP bounded read")
        data(call("close_file"))
        data(call("open_file", {"path": "long.log"}))
        wait_index(call)
        oversized_reply = call("read_lines", {"start_line": 1, "line_count": 16384})
        check(oversized_reply["result"]["isError"] and oversized_reply["result"]["structuredContent"]["error"] == "mcp-response-byte-budget", "Explicit wire budget error without source truncation")
        before = wire(path="/healthz", method="GET")[1]["cancelledRequests"]
        message = modern("tools/call", {"name": "search_lines", "arguments": {
            "pattern": "^(x|xx)*[YZ]$", "regex": True, "start_line": 1, "max_scan_lines": 2**64 - 1}})
        body = json.dumps(message).encode()
        connection = socket.create_connection(("127.0.0.1", port), timeout=5)
        header = (f"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nAuthorization: Bearer {token}\r\n"
                  f"Accept: application/json, text/event-stream\r\nContent-Type: application/json\r\n"
                  f"MCP-Protocol-Version: 2026-07-28\r\nMcp-Method: tools/call\r\nMcp-Name: search_lines\r\nContent-Length: {len(body)}\r\n\r\n").encode()
        connection.sendall(header + body)
        received = b""
        while b"\r\n\r\n" not in received:
            received += connection.recv(4096)
        check(b"text/event-stream" in received, "Cancellable request-scoped response stream")
        connection.close()
        deadline = time.monotonic() + 10
        while True:
            health = wire(path="/healthz", method="GET")[1]
            if health["activeRequests"] == 0 and health["cancelledRequests"] > before:
                break
            check(time.monotonic() < deadline, f"Disconnected query cancelled and joined: {health}, cancelledBefore={before}")
            time.sleep(0.02)
        check(data(call("read_lines", {"start_line": 1, "line_count": 1}))["returned_lines"] == 1, "Query permit recovered after cancellation")
        data(call("close_file"))
        status, _ = wire(path="/shutdown", method="POST")
        check(status == 200, "Authenticated graceful shutdown")
        process.wait(timeout=15)
        check(process.returncode == 0, "HTTP graceful process exit")
    finally:
        if process.poll() is None:
            try:
                wire(path="/shutdown", method="POST")
                process.wait(timeout=15)
            except Exception:
                process.kill()
                process.wait(timeout=5)
        log.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--work-root", required=True)
    parser.add_argument("--long-index", action="store_true")
    arguments = parser.parse_args()
    parent = pathlib.Path(arguments.work_root).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(tempfile.mkdtemp(prefix="v8trace-mcp-test-", dir=parent)).resolve()
    try:
        source = root / "sources"
        source.mkdir()
        (source / "lines.log").write_bytes(b"alpha\r\nbeta\xff\nlast\n")
        (root / "outside.log").write_bytes(b"outside")
        with (source / "long.log").open("wb") as output:
            block = (b"x" * 1023 + b"\n") * 1024
            for _ in range(128):
                output.write(block)
        capture = source / "capture"
        (capture / "events").mkdir(parents=True)
        (capture / "blobs/sha256").mkdir(parents=True)
        (capture / "summary.json").write_text(json.dumps({"captureConclusion": "completed", "integrityConclusion": "complete"}), encoding="utf-8")
        binary = b"\x00\xffbinary"
        digest = hashlib.sha256(binary).hexdigest()
        (capture / "blobs/sha256" / digest).write_bytes(binary)
        reference = {"path": "blobs/sha256/" + digest, "sha256": digest, "bytes": len(binary)}
        (capture / "blob.json").write_text(json.dumps(reference), encoding="utf-8")
        rows = [{"category": "jscall", "kind": "call-enter", "payload": {
            "scriptUrl": "https://fixture.test/计算.js", "functionName": "计算", "callId": "7",
            "source": reference}}, {"category": "jscall", "kind": "call-exit", "payload": {
            "scriptUrl": "https://fixture.test/计算.js", "callId": "7"}}]
        (capture / "events/jscall.jsonl").write_text("".join(json.dumps(row, ensure_ascii=False) + "\n" for row in rows), encoding="utf-8", newline="")
        causal_fixture(source)
        originals = {str(file.relative_to(source)): hashlib.sha256(file.read_bytes()).hexdigest()
                     for file in source.rglob("*") if file.is_file()}
        command = [str(pathlib.Path(arguments.runtime).resolve()), "mcp", "--source-root", str(source)]
        stdio_index = index_path(root, "stdio-index", arguments.long_index)
        http_index = index_path(root, "http-index", arguments.long_index)
        stdio_test(command, root, source, stdio_index)
        http_test(command, root, http_index)
        for relative, digest in originals.items():
            check(hashlib.sha256((source / relative).read_bytes()).hexdigest() == digest, "Source unchanged: " + relative)
        print(json.dumps({"status": "passed", "checks": CHECKS, "runtime": "cpp",
                          "transports": ["stdio", "http"], "sourceModified": False,
                          "longIndex": arguments.long_index,
                          "indexRootUtf16Units": [len(str(value).encode("utf-16-le")) // 2 for value in (stdio_index, http_index)]}))
    finally:
        if root.parent != parent or not root.name.startswith("v8trace-mcp-test-"):
            raise RuntimeError("Test cleanup boundary changed")
        shutil.rmtree(physical_path(root))


if __name__ == "__main__":
    main()
