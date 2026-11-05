#ifndef SERVICES_NETWORK_V8TRACE_HTTP_H_
#define SERVICES_NETWORK_V8TRACE_HTTP_H_

#include "base/trace_event/v8trace-browser-record.h"
#include "base/strings/string_util.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_raw_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/url_request/url_request.h"
#include "net/url_request/v8trace-redirect-body.h"
#include "services/network/public/cpp/resource_request_body.h"

namespace network::v8trace_http {
struct State { std::string id, upload; uint64_t bytes = 0, wire_headers = 0; bool finished = false; };
inline auto& States() {
  static thread_local std::unordered_map<const void*, State> states;
  return states;
}
inline bool Enabled() { return v8trace_browser::Enabled("disabled-by-default-v8.http"); }
inline base::DictValue Headers(const net::HttpRequestHeaders& headers) {
  base::DictValue result;
  for (net::HttpRequestHeaders::Iterator it(headers); it.GetNext();) result.Set(it.name(), it.value());
  return result;
}
inline State& Begin(net::URLRequest* request) {
  auto& state = States()[request];
  if (!Enabled() || !request->url().SchemeIsHTTPOrHTTPS() || !state.id.empty()) return state;
  static thread_local uint64_t requests = 0;
  state.id = "network:" + v8trace_browser::Address(request) + ":" + base::NumberToString(++requests);
  net::v8trace_redirect::BindParent(request, state.id);
  base::DictValue record;
  record.Set("kind", "request"); record.Set("requestId", state.id); record.Set("layer", "network");
  record.Set("url", request->url().spec()); record.Set("method", request->method());
  record.Set("headers", Headers(request->extra_request_headers()));
  record.Set("uploadStreamId", state.upload); record.Set("requestBodyPresent", !state.upload.empty());
  base::DictValue net_log_source;
  net_log_source.Set("id", base::NumberToString(request->net_log().source().id));
  net_log_source.Set("type", static_cast<int>(request->net_log().source().type));
  record.Set("netLogSource", std::move(net_log_source));
  v8trace_browser::Http(std::move(record));
  return state;
}
inline void RequestSource(const State& state, const ResourceRequestBody* body) {
  if (!body || state.id.empty()) return;
  base::ListValue elements;
  uint64_t total = 0;
  bool all_bytes = true;
  for (const auto& element : *body->elements()) {
    base::DictValue item;
    item.Set("index", static_cast<int>(elements.size()));
    switch (element.type()) {
      case DataElement::Tag::kBytes:
        item.Set("type", "bytes");
        item.Set("bytes", base::NumberToString(element.As<DataElementBytes>().bytes().size()));
        total += element.As<DataElementBytes>().bytes().size();
        break;
      case DataElement::Tag::kFile:
        item.Set("type", "file"); all_bytes = false; break;
      case DataElement::Tag::kDataPipe:
        item.Set("type", "data-pipe"); all_bytes = false; break;
      case DataElement::Tag::kChunkedDataPipe:
        item.Set("type", "chunked-data-pipe"); all_bytes = false; break;
    }
    elements.Append(std::move(item));
  }
  base::DictValue start;
  start.Set("kind", "request-source-start"); start.Set("requestId", state.id);
  start.Set("bodyProtocol", "v8trace/request-source/1");
  start.Set("observation", "request-source-before-upload-init");
  start.Set("elements", std::move(elements)); start.Set("allBytes", all_bytes);
  if (all_bytes) start.Set("expectedBytes", base::NumberToString(total));
  else start.Set("expectedBytes", base::Value());
  v8trace_browser::Http(std::move(start));
  uint64_t offset = 0;
  if (all_bytes) for (const auto& element : *body->elements()) {
    const auto& bytes = element.As<DataElementBytes>().bytes();
    v8trace_browser::Chunk(state.id, "request-source", offset, base::span<const uint8_t>(bytes), "network-request-source");
    offset += bytes.size();
  }
  base::DictValue end;
  end.Set("kind", "request-source-end"); end.Set("requestId", state.id);
  end.Set("bodyProtocol", "v8trace/request-source/1");
  end.Set("complete", all_bytes); end.Set("bytes", base::NumberToString(offset));
  v8trace_browser::Http(std::move(end));
}
inline void Bind(net::URLRequest* request, const void* upload,
                 const ResourceRequestBody* body = nullptr) {
  if (!Enabled()) return;
  States()[request].upload = v8trace_browser::Address(upload);
  const auto& state = Begin(request);
  RequestSource(state, body);
}
inline void RequestHeaders(net::URLRequest* request, const net::HttpRequestHeaders& headers) {
  if (!Enabled()) return;
  const auto& state = Begin(request); if (state.id.empty()) return;
  base::DictValue record; record.Set("kind", "request-headers"); record.Set("requestId", state.id);
  record.Set("headers", Headers(headers)); v8trace_browser::Http(std::move(record));
}
inline std::string HeaderBytes(std::string_view bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string encoded; encoded.reserve(bytes.size() * 2);
  for (unsigned char value : bytes) { encoded += digits[value >> 4]; encoded += digits[value & 15]; }
  return encoded;
}
inline void WireRequestHeaders(net::URLRequest* request, const net::HttpRawRequestHeaders& headers) {
  if (!Enabled()) return;
  auto& state = Begin(request); if (state.id.empty()) return;
  base::DictValue record, fields; base::ListValue pairs;
  record.Set("kind", "request-wire-headers"); record.Set("requestId", state.id);
  record.Set("wireHeadersProtocol", "v8trace/http-wire-headers/1");
  record.Set("wireAttempt", base::NumberToString(++state.wire_headers));
  record.Set("requestLine", headers.request_line()); record.Set("requestLineBytesHex", HeaderBytes(headers.request_line()));
  for (const auto& [name, value] : headers.headers()) {
    base::DictValue pair; pair.Set("name", name);
    if (base::IsStringUTF8(value)) { fields.Set(name, value); pair.Set("value", value); }
    else { fields.Set(name, base::Value()); pair.Set("value", base::Value()); }
    pair.Set("nameBytesHex", HeaderBytes(name)); pair.Set("valueBytesHex", HeaderBytes(value)); pairs.Append(std::move(pair));
  }
  record.Set("headers", std::move(fields)); record.Set("headerPairs", std::move(pairs));
  v8trace_browser::Http(std::move(record));
}
inline void Response(net::URLRequest* request, bool encoded) {
  if (!Enabled()) return;
  const auto& state = Begin(request); if (state.id.empty()) return;
  base::DictValue record; record.Set("kind", "response"); record.Set("requestId", state.id);
  record.Set("url", request->url().spec());
  auto* headers = request->response_headers();
  base::DictValue fields; base::ListValue pairs;
  if (headers) {
    record.Set("status", headers->response_code()); record.Set("statusText", headers->GetStatusText());
    record.Set("rawHeaders", headers->raw_headers());
    size_t iterator = 0; std::string name, value;
    while (headers->EnumerateHeaderLines(&iterator, &name, &value)) {
      fields.Set(name, value); base::DictValue pair; pair.Set("name", name); pair.Set("value", value); pairs.Append(std::move(pair));
    }
  }
  record.Set("headers", std::move(fields)); record.Set("headerPairs", std::move(pairs));
  record.Set("bodyRepresentation", encoded ? "encoded-network-bytes" : "decoded-browser-bytes");
  v8trace_browser::Http(std::move(record));
}
inline void Data(net::URLRequest* request, base::span<const uint8_t> bytes) {
  if (!Enabled()) return;
  auto& state = Begin(request); if (state.id.empty()) return;
  v8trace_browser::Chunk(state.id, "response", state.bytes, bytes, "network"); state.bytes += bytes.size();
}
inline void Finish(net::URLRequest* request, int error, const char* reason = "url-loader-completed") {
  if (!Enabled()) return;
  auto& state = Begin(request); if (state.id.empty() || state.finished) return;
  state.finished = true;
  base::DictValue record; record.Set("kind", "complete"); record.Set("requestId", state.id);
  record.Set("success", error == 0); record.Set("errorCode", error); record.Set("bytes", base::NumberToString(state.bytes));
  record.Set("termination", reason);
  record.Set("wireHeadersProtocol", "v8trace/http-wire-headers/1");
  record.Set("wireHeaderCount", base::NumberToString(state.wire_headers)); record.Set("wasCached", request->was_cached());
  record.Set("networkAccessed", request->response_info().network_accessed);
  v8trace_browser::Http(std::move(record));
}
inline void Redirect(net::URLRequest* request, const std::string& next, const std::string& method) {
  if (!Enabled()) return;
  const auto* body = net::v8trace_redirect::Current(request);
  Response(request, body != nullptr); const auto& state = Begin(request);
  base::DictValue record; record.Set("kind", "redirect"); record.Set("requestId", state.id); record.Set("nextUrl", next);
  if (body) { record.Set("redirectBodyId", body->id); record.Set("redirectBodyProtocol", net::v8trace_redirect::kProtocol); }
  v8trace_browser::Http(std::move(record)); Finish(request, 0);
  auto upload = (method == "GET" || method == "HEAD") ? std::string() : state.upload;
  net::v8trace_redirect::ForgetParent(request);
  States().erase(request); States()[request].upload = upload;
}
inline void Release(net::URLRequest* request) {
  auto found = States().find(request);
  if (found != States().end() && !found->second.id.empty() && !found->second.finished)
    Finish(request, net::ERR_ABORTED, "url-loader-destroyed");
  if (request) net::v8trace_redirect::ForgetParent(request);
  States().erase(request);
}
}
#endif
