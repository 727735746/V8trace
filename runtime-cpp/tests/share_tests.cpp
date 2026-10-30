#include "v8trace/share.h"
#include <algorithm>
#include <iostream>
#include <sstream>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
template<class F> void reject(F action, const char* message) { bool failed = false; try { action(); } catch (...) { failed = true; } check(failed, message); }
Json settings(bool opaque = false) { return {{"protocol", "v8trace/share-fields/1"}, {"copyOpaque", opaque}, {"rules", Json::array({{{"field", "Authorization"}, {"action", "replace"}, {"value", "[字段已处理]"}}, {{"field", "password"}, {"action", "remove"}}})}}; }
std::map<fs::path, std::string> snapshot(const fs::path& root) { std::map<fs::path, std::string> files; for (auto& entry : fs::recursive_directory_iterator(root)) if (entry.is_regular_file()) files[entry.path().lexically_relative(root)] = file_sha256(entry.path()); return files; }
}
int main() {
    auto root = fs::temp_directory_path() / ("v8trace-share-tests-" + uuid()); fs::create_directory(root);
    try {
        check(share_field_settings(settings()).at("rules").at(0).at("field") == "authorization", "ASCII field names normalized, not fuzzy patterns");
        auto bad = settings(); bad["rules"].push_back(bad["rules"].at(0)); reject([&] { share_field_settings(bad); }, "duplicate normalized rules rejected");
        bad = settings(); bad["rules"][0]["action"] = "evaluate"; reject([&] { share_field_settings(bad); }, "no executable field rules");
        bad = settings(); bad["extra"] = true; reject([&] { share_field_settings(bad); }, "unknown field settings rejected");
        std::string bytes(196613, 'x'); bytes.back() = '\0'; size_t offset = 0;
        check(reader_sha256([&](char* target, size_t capacity) { auto count = std::min(capacity, bytes.size() - offset); std::copy_n(bytes.data() + offset, count, target); offset += count; return count; }) == sha256(bytes), "bounded incremental reader SHA-256 exact");
        reject([&] { reader_sha256([](char*, size_t size) { return size + 1; }); }, "invalid reader count rejected");
#ifdef _WIN32
        auto source = root / "source"; fs::create_directories(source / "events"); fs::create_directories(source / "blobs/sha256");
        write_json(source / "summary.json", {{"schemaVersion", 1}, {"backend", "chromium-native"}, {"integrityConclusion", "complete"}, {"captureConclusion", "collected"}});
        write_json(source / "a.json", {{"Authorization", "original-secret"}, {"nested", Json::array({{{"password", "secret"}, {"safe", "中文😀"}}, {{"authorization", {{"nested", "entire-value"}}}}})}, {"embedded", "{\"password\":\"not-JSON-field\"}"}, {"reference", {{"path", "../../not-followed"}, {"sha256", std::string(64, 'a')}, {"bytes", 1}}}});
        write_file(source / "bad.json", "{unfinished\n", true);
        write_json(source / "b-long-key.json", {{std::string(6000, 'k'), {{"authorization", "observed-full-value"}}}, {"max", UINT64_MAX}});
        write_file(source / "precision.json", "{\"id\":18446744073709551616}\n", true);
        write_file(source / "large.json", std::string(1048577, ' '), true);
        write_file(source / "oversized.jsonl", "{\"authorization\":\"first-row-not-exported\"}\n{\"token\":\"" + std::string(1048577, 'x') + "\"}\n", true);
        write_file(source / "blobs/sha256" / std::string(64, 'a'), std::string(2 * 1048576, '\xff'), true);
        { std::ofstream out(source / "events/large.jsonl", std::ios::binary); auto line = dump(Json{{"authorization", "full-value"}, {"keep", std::string(200, 'z')}}) + "\n"; for (int i = 0; i < 70000; ++i) out << line; }
        auto before = snapshot(source); auto preview = preview_share_copy(source, settings());
        check(preview.at("affectedFieldCounts").at("authorization") == 70003 && preview.at("affectedFieldCounts").at("password") == 1, "nested objects/arrays and large JSONL coverage counted without whole-file reads");
        check(preview.at("affectedPathsSample").size() == 100 && preview.at("dispositions").at("omitted") == 5, "bounded preview and explicit malformed/large/opaque/precision omissions");
        check(dump(preview).size() < 1048576 && std::any_of(preview.at("affectedPathsSample").begin(), preview.at("affectedPathsSample").end(), [](const Json& item) { return text(item).find("display boundary") != std::string::npos; }), "long member paths are explicitly bounded without losing counts");
        auto output = root / "copy"; auto result = create_share_copy(source, output, settings(), str(preview, "previewBinding"));
        check(result.at("state") == "complete-derived-copy" && snapshot(source) == before, "independent copy leaves every original byte and filename unchanged");
        auto transformed = load_json(output / "content/a.json");
        check(load_json(output / "content/b-long-key.json").at("max") == UINT64_MAX && !fs::exists(output / "content/precision.json"), "uint64 remains exact; unsupported large integer is omitted instead of rounded");
        check(transformed.at("Authorization") == "[字段已处理]" && !transformed.at("nested").at(0).contains("password") && transformed.at("nested").at(1).at("authorization") == "[字段已处理]", "exact recursive replacement/removal with original casing retained");
        check(transformed.at("embedded") == "{\"password\":\"not-JSON-field\"}" && transformed.at("reference").at("path") == "../../not-followed", "embedded strings and reference text are not guessed or followed");
        auto manifest = load_json(output / "share.manifest.json");
        check(manifest.at("sourceSnapshotVerified") == true && manifest.at("coverage").at("notOriginalCapture") == true && !fs::exists(output / "summary.json"), "derived copy is not presented as a raw complete capture");
        check(load_json(output / "content/summary.json").at("integrityConclusion") == "derived-not-assessed", "derived summary explicitly invalidates raw completeness");
        for (const auto& file : manifest.at("files")) { check(before.at(path(str(file, "sourcePath"))) == str(file, "sourceSha256"), "manifest source hash exact"); if (file.contains("copyPath")) check(file_sha256(output / path(str(file, "copyPath"))) == str(file, "copySha256"), "manifest copy hash exact"); }
        check(!fs::exists(output / "content/bad.json") && !fs::exists(output / "content/large.json") && !fs::exists(output / "content/oversized.jsonl") && !fs::exists(output / "content/blobs"), "unsupported files are wholly omitted, including valid prefixes before oversized JSONL records");
        reject([&] { create_share_copy(source, output, settings(), str(preview, "previewBinding")); }, "existing destination never overwritten");
        reject([&] { create_share_copy(source, source / "nested-copy", settings(), str(preview, "previewBinding")); }, "copy inside original rejected");
        reject([&] { preview_share_copy(source / ".." / "source", settings()); }, "parent traversal rejected before path resolution");
        write_file(source / "changed.json", "{}\n", true); reject([&] { create_share_copy(source, root / "stale", settings(), str(preview, "previewBinding")); }, "changed source inventory invalidates reviewed binding"); fs::remove(source / "changed.json");
        auto changed_rules = settings(); changed_rules["rules"][0]["value"] = "different";
        reject([&] { create_share_copy(source, root / "changed-rules", changed_rules, str(preview, "previewBinding")); }, "changed rules require fresh preview");
        check(!fs::exists(root / "stale") && !fs::exists(root / "changed-rules"), "stale preview never creates destination");
        auto with_opaque = preview_share_copy(source, settings(true)); auto opaque_output = root / "opaque-copy";
        create_share_copy(source, opaque_output, settings(true), str(with_opaque, "previewBinding"));
        check(file_sha256(opaque_output / "content/blobs/sha256" / std::string(64, 'a')) == before.at(path("blobs/sha256/" + std::string(64, 'a'))), "explicit opaque copy is byte-exact, with no binary transformation");
        std::atomic_bool cancel = true; reject([&] { create_share_copy(source, root / "early-cancel", settings(), str(preview, "previewBinding"), {}, &cancel); }, "early cancellation honored");
        check(!fs::exists(root / "early-cancel"), "cancelled preview creates no output"); cancel = false;
        auto cancelled = root / "cancelled"; reject([&] { create_share_copy(source, cancelled, settings(), str(preview, "previewBinding"), [&](const Json& status) { if (status.contains("copiedFiles")) cancel = true; }, &cancel); }, "copy cancellation honored between streaming units");
        check(load_json(cancelled / "share.failed.json").at("state") == "cancelled" && !fs::exists(cancelled / "share.manifest.json"), "cancelled partial copy retained with distinct failure record, never complete");
        auto failed = root / "failed"; reject([&] { create_share_copy(source, failed, settings(), str(preview, "previewBinding"), [&](const Json& status) { if (status.contains("copiedFiles")) throw std::runtime_error("injected observer failure"); }); }, "injected mid-copy failure retained");
        check(load_json(failed / "share.failed.json").at("partialDestinationRetained") == true && !fs::exists(failed / "share.manifest.json") && snapshot(source) == before, "failed copy is separate and originals remain exact");
#else
        reject([&] { preview_share_copy(root, settings()); }, "unimplemented secure platform backend is explicit");
#endif
        check(fs::canonical(root.parent_path()) == fs::canonical(fs::temp_directory_path()) && utf8(root.filename()).starts_with("v8trace-share-tests-"), "owned temporary cleanup boundary");
        fs::remove_all(root); std::cout << "PASS " << checks << " independent sharing-copy checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nOwned failure fixture retained: " << utf8(root) << '\n'; return 1; }
}
