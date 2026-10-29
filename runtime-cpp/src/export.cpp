#include "v8trace/core.h"
#include "v8trace/index.h"
#include <stdexcept>

namespace v8trace {
namespace {
Json copy_reference(Json blob) { if (blob.is_object()) blob["path"] = "files/" + str(blob, "sha256"); return blob; }
Json decoded(const Json& row, size_t column = 0) { return parse(text(row.at(column))); }
Json identifiers(DiskIndex& index, std::string_view query, const Json& parameters) {
  Json output = Json::array(); index.each(query, parameters, [&](const Json& row) { output.push_back(row.at(0)); }); return output;
}
}
Json inspect_capture(const fs::path& directory) {
  auto root = fs::canonical(directory); auto summary = load_json(root / "summary.json", 8388608);
  if (field(summary, "schemaVersion") != 1 || !std::set<std::string>{"chrome-cdp", "chromium-opcode", "chrome-native", "chromium-native"}.contains(str(summary, "backend"))) throw std::runtime_error("Unsupported capture summary");
  return {{"root", utf8(root)}, {"summary", summary}};
}

Json export_case(const fs::path& directory, const fs::path& destination) {
  auto capture = inspect_capture(directory); auto root = fs::canonical(directory); auto output = fs::absolute(destination).lexically_normal();
  if (fs::exists(output)) throw std::runtime_error("Export output already exists");
  auto gap_journal = field(capture.at("summary"), "gapJournal");
  if (!gap_journal.is_null() && (str(gap_journal, "protocol") != "v8trace/gaps/1" || str(gap_journal, "path") != "gaps.jsonl" || !safe_uint(field(gap_journal, "bytes")) || !fs::is_regular_file(root / "gaps.jsonl") || !inside(root, fs::canonical(root / "gaps.jsonl")) || fs::file_size(root / "gaps.jsonl") != gap_journal.at("bytes").get<uint64_t>() || file_sha256(root / "gaps.jsonl") != str(gap_journal, "sha256"))) throw std::runtime_error("Gap journal integrity mismatch");
  fs::create_directories(output.parent_path()); DiskIndex index(output.parent_path());
  index.exec("CREATE TABLE blobs(hash BLOB PRIMARY KEY,file BLOB,bytes INTEGER) WITHOUT ROWID;"
             "CREATE TABLE network(seq INTEGER PRIMARY KEY,data BLOB);"
             "CREATE TABLE scripts(seq INTEGER PRIMARY KEY,url BLOB,module INTEGER,data BLOB); CREATE INDEX scripts_url ON scripts(url,module);"
             "CREATE TABLE candidates(seq INTEGER PRIMARY KEY,scope BLOB,data BLOB); CREATE INDEX candidates_scope ON candidates(scope);"
             "CREATE TABLE omissions(seq INTEGER PRIMARY KEY,data BLOB);"
             "CREATE TABLE dependencies(scope BLOB PRIMARY KEY,data BLOB) WITHOUT ROWID;"
             "CREATE TABLE contexts(dependency BLOB,context BLOB,PRIMARY KEY(dependency,context)) WITHOUT ROWID");
  auto check_reference = [&](const Json& blob) {
    if (blob.is_null()) return;
    auto digest = str(blob, "sha256"); if (digest.size() != 64 || str(blob, "path").empty() || !safe_uint(field(blob, "bytes"))) throw std::runtime_error("Invalid blob reference");
    auto file = fs::canonical(root / path(str(blob, "path"))); auto bytes = blob.at("bytes").get<uint64_t>();
    if (!inside(root, file) || fs::file_size(file) != bytes) throw std::runtime_error("Blob integrity mismatch");
    auto seen = index.one("SELECT file,bytes FROM blobs WHERE hash=?", Json::array({digest}));
    if (!seen || text(seen->at(0)) != utf8(file) || seen->at(1).get<uint64_t>() != bytes) if (file_sha256(file) != digest) throw std::runtime_error("Blob integrity mismatch");
    index.run("INSERT OR IGNORE INTO blobs VALUES(?,?,?)", Json::array({digest, utf8(file), bytes}));
  };
  lines(root / "catalog/resources.jsonl", [&](Json row) {
    for (auto key : {"body", "postBody", "encodedBody", "bodyPrefix"}) { if (std::string_view(key)=="bodyPrefix"&&!row.contains(key)) continue; check_reference(field(row, key)); row[key] = copy_reference(field(row, key)); }
    index.run("INSERT INTO network(data) VALUES(?)", Json::array({row}));
  });
  lines(root / "catalog/scripts.jsonl", [&](Json row) {
    for (auto key : {"source", "sourceUtf16", "bytecode"}) { check_reference(field(row, key)); row[key] = copy_reference(field(row, key)); }
    index.run("INSERT INTO scripts(url,module,data) VALUES(?,?,?)", Json::array({str(row, "url"), field(row, "isModule", false) == true ? 1 : 0, row}));
  });
  std::function<void(const Json&)> audit_blobs = [&](const Json& value) {
    if (value.is_object()) { if (value.contains("sha256") && value.contains("path") && value.contains("bytes") && str(value,"path").starts_with("blobs/sha256/")) check_reference(value); for(const auto& child:value)audit_blobs(child); }
    else if(value.is_array())for(const auto& child:value)audit_blobs(child);
  };
  auto audit_journal=[&](const fs::path& file){if(!inside(root,fs::canonical(file)))throw std::runtime_error("Evidence journal leaves capture");std::ifstream source(file,std::ios::binary);std::string row;while(std::getline(source,row))if(row.find("blobs/sha256/")!=row.npos)audit_blobs(parse(row));if(!source.eof())throw std::runtime_error("Read evidence journal failed");};
  for(auto directory:{root/"events",root/"catalog"})if(fs::is_directory(directory))for(const auto& entry:fs::directory_iterator(directory))if(entry.is_regular_file()&&entry.path().extension()==".jsonl")audit_journal(entry.path());
  if(fs::exists(root/"gaps.jsonl"))audit_journal(root/"gaps.jsonl");
  index.checkpoint();
  const std::map<std::string, std::string> types{{"Document", "document"}, {"Script", "script"}, {"Stylesheet", "stylesheet"}, {"Font", "font"}, {"Fetch", "fetch"}, {"XHR", "fetch"}, {"Other", "fetch"}, {"Wasm", "wasm"}};
  auto omission = [&](const Json& row) { index.run("INSERT INTO omissions(data) VALUES(?)", Json::array({row})); };
  index.each("SELECT data FROM network ORDER BY seq", Json::array(), [&](const Json& item) {
    auto row = decoded(item); auto request = field(row, "request"), response = field(row, "response"), body = field(row, "body"); auto type = str(row, "type"), url = str(request, "url");
    if (!types.contains(type) || (!url.starts_with("https://") && !url.starts_with("http://")) || str(request, "method") != "GET" || body.is_null() || str(row, "conclusion") != "complete" || !safe_uint(field(response, "status")) || response.at("status") < 100 || response.at("status") > 599) { omission({{"url", url}, {"requestId", field(row, "requestId")}, {"reason", "not-a-complete-supported-static-resource"}}); return; }
    auto mime = str(response, "mimeType");
    if (type == "Document" && !std::set<std::string>{"application/xhtml+xml", "application/xml", "image/svg+xml", "text/html", "text/xml"}.contains(mime)) { omission({{"url", url}, {"reason", "unsupported-document-mime"}}); return; }
    Json headers = Json::object(); auto source_headers = field(response, "headers", Json::object());
    if (source_headers.is_object()) for (auto it = source_headers.begin(); it != source_headers.end(); ++it) { auto key = lower(it.key()); if (key != "content-encoding" && key != "content-length" && key != "transfer-encoding") headers[key] = it.value(); }
    Json resource{{"url", url}, {"type", types.at(type)}, {"path", str(body, "path")}, {"sha256", str(body, "sha256")}, {"mimeType", mime}, {"responseStatus", response.at("status")}, {"responseHeaders", headers}};
    if (type == "Script") {
      auto modes = identifiers(index, "SELECT DISTINCT module FROM scripts WHERE url=? ORDER BY module LIMIT 2", Json::array({url}));
      if (modes.size() > 1) { omission({{"url", url}, {"reason", "ambiguous-module-mode"}}); return; }
      resource["module"] = modes.size() == 1 && modes.at(0) == 1;
    }
    index.run("INSERT INTO candidates(scope,data) VALUES(?,?)", Json::array({types.at(type) + '\0' + url, resource}));
  });
  lines(root / "events/env.jsonl", [&](const Json& event) {
    auto kind = str(event, "kind"); if (kind != "enter" && kind != "return" && kind != "throw") return;
    auto row = field(event, "payload"), key = Json(str(row, "binding") + '/' + str(row, "operation"));
    auto found = index.one("SELECT data FROM dependencies WHERE scope=?", Json::array({key}));
    Json dependency = found ? decoded(*found) : Json{{"binding", field(row, "binding")}, {"operation", field(row, "operation")}, {"enter", 0}, {"return", 0}, {"throw", 0}};
    dependency[kind] = dependency.at(kind).get<uint64_t>() + 1;
    index.run("INSERT OR REPLACE INTO dependencies VALUES(?,?)", Json::array({key, dependency}));
    index.run("INSERT OR IGNORE INTO contexts VALUES(?,?)", Json::array({key, dump(Json::array({field(event, "sessionId"), field(event, "processId"), field(event, "isolateAddress"), field(event, "realmId"), field(event, "contextId")}))}));
  });
  index.checkpoint();
  if (!fs::create_directory(output)) throw std::runtime_error("Export creation race"); fs::create_directory(output / "files");
  fs::create_directories(output / "blobs/sha256");
  index.each("SELECT hash,file,bytes FROM blobs ORDER BY hash", Json::array(), [&](const Json& row) {
    auto target = output / "files" / path(text(row.at(0))); fs::copy_file(path(text(row.at(1))), target);
    auto mirror=output/"blobs/sha256"/path(text(row.at(0)));std::error_code error;fs::create_hard_link(target,mirror,error);if(error)fs::copy_file(target,mirror);
    if (fs::file_size(target) != row.at(2).get<uint64_t>() || file_sha256(target) != text(row.at(0))) throw std::runtime_error("Exported blob changed during copy");
  });
  auto write = [&](std::string_view name, const Json& value) { write_json(output / path(name), value); };
  JsonArrayOutput scripts(output / "scripts.json");
  index.each("SELECT data FROM scripts ORDER BY seq", Json::array(), [&](const Json& row) { scripts.raw_item(text(row.at(0))); }); scripts.finish();
  std::ofstream replay(output / "replay.network.jsonl", std::ios::binary); uint64_t network_count = 0;
  index.each("SELECT data FROM network ORDER BY seq", Json::array(), [&](const Json& row) { replay << text(row.at(0)) << '\n'; if (!replay) throw std::runtime_error("Export network journal write failed"); ++network_count; });
  replay.flush(); if (!replay) throw std::runtime_error("Export network journal flush failed");
  JsonArrayOutput resources(output / "resources.json", "{\"schemaVersion\":1,\"resources\":[");
  JsonArrayOutput conflicts(output / "resource.conflicts.json");
  index.each("SELECT scope,COUNT(DISTINCT data) FROM candidates GROUP BY scope ORDER BY scope", Json::array(), [&](const Json& group) {
    auto first = index.one("SELECT data FROM candidates WHERE scope=? ORDER BY seq LIMIT 1", Json::array({group.at(0)}));
    if (group.at(1) == 1) resources.raw_item(text(first->at(0)));
    else {
      auto row = decoded(*first); conflicts.begin_item();
      conflicts.fragment("{\"url\":" + dump(row.at("url")) + ",\"type\":" + dump(row.at("type")) + ",\"candidates\":["); bool separator = false;
      index.each("SELECT data FROM candidates WHERE scope=? ORDER BY seq", Json::array({group.at(0)}), [&](const Json& candidate) { if (separator) conflicts.fragment(","); separator = true; conflicts.fragment(text(candidate.at(0))); });
      conflicts.fragment("]}");
    }
  });
  resources.finish("]}\n"); conflicts.finish();
  JsonArrayOutput omissions(output / "resource.omissions.json");
  index.each("SELECT data FROM omissions ORDER BY seq", Json::array(), [&](const Json& row) { omissions.raw_item(text(row.at(0))); }); omissions.finish();
  JsonArrayOutput dependencies(output / "env.requirements.json", "{\"observation\":\"diagnostic-js-hooks\",\"dependencies\":[");
  index.each("SELECT scope,data FROM dependencies ORDER BY scope", Json::array(), [&](const Json& row) {
    auto value = decoded(row, 1); value["contexts"] = identifiers(index, "SELECT context FROM contexts WHERE dependency=? ORDER BY context", Json::array({row.at(0)})); dependencies.append(value);
  });
  dependencies.finish(fs::exists(root / "native-env-summary.json") ? "],\"completeEnvironmentModel\":false,\"nativeEvidence\":{\"summary\":\"native-env-summary.json\",\"inventory\":\"env-inventory.jsonl\",\"events\":\"env.jsonl\",\"coverage\":\"observations-not-complete-environment\"}}\n" : "],\"completeEnvironmentModel\":false}\n");
  write("capture.reference.json", {{"captureId", capture["summary"]["captureId"]}, {"sourceDirectory", utf8(root)}, {"summarySha256", file_sha256(root / "summary.json")}, {"captureSummary", capture["summary"]}});
  for (auto channel : {"opcode", "native", "jscall", "wasm", "env", "console", "exception", "websocket", "indexeddb", "cookie", "storage"}) if (fs::exists(root / "events" / (std::string(channel) + ".jsonl"))) fs::copy_file(root / "events" / (std::string(channel) + ".jsonl"), output / (std::string(channel) + ".jsonl"));
  for (auto name : {"detail-summary.json", "opcode-summary.json", "correlation-summary.json", "native-env-summary.json", "native-env-producers.jsonl", "closure-summary.json", "gaps.jsonl", "gaps.json"}) if (fs::exists(root / name)) fs::copy_file(root / name, output / name);
  for (auto name : {"native-state-summary.json", "native-state-sessions.jsonl", "native-state-producers.jsonl", "state-correlation-summary.json"}) if (fs::exists(root / name)) fs::copy_file(root / name, output / name);
  for (auto name : {"state-operations", "state-chunks", "state-correlations"}) if (fs::exists(root / "catalog" / (std::string(name) + ".jsonl"))) fs::copy_file(root / "catalog" / (std::string(name) + ".jsonl"), output / (std::string(name) + ".jsonl"));
  for (auto name : {"wasm-modules", "wasm-module-index", "vmp-probes", "vmp-instructions"}) if (fs::exists(root / "catalog" / (std::string(name) + ".jsonl"))) fs::copy_file(root / "catalog" / (std::string(name) + ".jsonl"), output / (std::string(name) + ".jsonl"));
  if(fs::exists(root/"vmp-summary.json"))fs::copy_file(root/"vmp-summary.json",output/"vmp-summary.json");
  if(fs::exists(root/"vmp-probe-spec.json"))fs::copy_file(root/"vmp-probe-spec.json",output/"vmp-probe-spec.json");
  for (auto channel : {"wasm-module", "vmp"}) if (fs::exists(root / "events" / (std::string(channel) + ".jsonl"))) fs::copy_file(root / "events" / (std::string(channel) + ".jsonl"), output / (std::string(channel) + ".jsonl"));
  for (auto name : {"calls", "correlations", "script-index", "hook-spans", "env-inventory"}) if (fs::exists(root / "catalog" / (std::string(name) + ".jsonl"))) fs::copy_file(root / "catalog" / (std::string(name) + ".jsonl"), output / (std::string(name) + ".jsonl"));
  write_file(output / "main.template.js", "\"use strict\";\nfunction main(context) { return { status: \"pending-entry\", resourceManifest: \"resources.json\" }; }\n", true);
  write_file(output / path("交接说明.md"), "# 采集证据交接\n\n本目录由 C++ 内核导出，所有说明使用 UTF-8 中文。\n\n- 原始实参、返回值、源码及 HTTP 正文通过 SHA-256 引用校验。\n- replay.network.jsonl 是网络证据，不是已完成的协议实现。\n- main.template.js 仅为待选业务入口，尚未执行补环境或纯算复现。\n- 请先检查 capture.reference.json 的缺口及完整性结论；partial 不代表复现成功。\n- 最终复现禁止依赖浏览器自动化、CDP 或浏览器代算，也不做自动化兜底。\n", true);
  Json summary{{"schemaVersion", 1}, {"captureId", capture["summary"]["captureId"]}, {"conclusion", "exported-pending-entry"}, {"resources", resources.count()}, {"networkRecords", network_count}, {"scripts", scripts.count()}, {"dependencies", dependencies.count()}, {"conflicts", conflicts.count()}, {"omissions", omissions.count()}, {"captureIntegrity", capture["summary"]["integrityConclusion"]}, {"diskIndex", index.stats()}};
  write("export.summary.json", summary); return summary;
}
}
