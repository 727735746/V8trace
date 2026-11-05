#ifndef CHROME_BROWSER_V8TRACE_RECORDING_CONTROL_H_
#define CHROME_BROWSER_V8TRACE_RECORDING_CONTROL_H_

#include <limits>
#include <optional>
#include <string>
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/process/process_handle.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/trace_event/trace_config.h"
#include "content/public/browser/tracing_controller.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/log/net_log_capture_mode.h"
#include "services/network/public/mojom/net_log.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "url/gurl.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace v8trace_recording {
network::mojom::NetworkContext* GetNetworkContext();
bool Navigate(const GURL& url);
bool DrainHttp(base::OnceCallback<void(bool)> done);
class Controller {
 public:
  static Controller& Get() { static base::NoDestructor<Controller> instance; return *instance; }
  Controller();
  ~Controller();
  void Start() {
    const auto* command = base::CommandLine::ForCurrentProcess();
    if (started_ || !command->HasSwitch("v8-trace-recording-control")) return;
    started_ = true; busy_ = true; config_file_ = command->GetSwitchValuePath("v8-trace-recording-control").NormalizePathSeparators();
    directory_ = config_file_.DirName();
    io_ = base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock(), base::TaskPriority::USER_VISIBLE, base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
    io_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&Read, config_file_), base::BindOnce(&Controller::Configure, base::Unretained(this)));
  }
  void Shutdown() {
    if (!started_ || state_ == "finished" || state_ == "error") return;
    shutting_down_ = true; timer_.Stop();
    base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
    finished_ = loop.QuitClosure();
    base::OneShotTimer deadline;
    deadline.Start(FROM_HERE, base::Seconds(110), base::BindOnce(&Controller::Fail, base::Unretained(this), "Browser shutdown recording flush timeout"));
    if (!busy_) Finish();
    loop.Run();
  }
 private:
  static constexpr char kProtocol[] = "v8trace/recording/1";
  static std::optional<base::DictValue> Read(const base::FilePath& file) {
    std::string bytes;
    if (file.ReferencesParent()) { base::DictValue error; error.Set("readError", true); return error; }
#if BUILDFLAG(IS_WIN)
    const auto deadline = base::TimeTicks::Now() + base::Seconds(2);
    for (;;) {
      HANDLE handle = ::CreateFileW(file.value().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      DWORD failure = ERROR_SUCCESS;
      if (handle == INVALID_HANDLE_VALUE) {
        failure = ::GetLastError();
        if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND) return std::nullopt;
      } else {
        LARGE_INTEGER length;
        if (!::GetFileSizeEx(handle, &length)) failure = ::GetLastError();
        else if (length.QuadPart < 0 || length.QuadPart > 1048576) failure = ERROR_FILE_TOO_LARGE;
        else {
          bytes.resize(static_cast<size_t>(length.QuadPart)); DWORD read = 0;
          if (!::ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) failure = ::GetLastError();
          else if (read != bytes.size()) failure = ERROR_HANDLE_EOF;
        }
        ::CloseHandle(handle);
      }
      if (!failure) break;
      if ((failure == ERROR_SHARING_VIOLATION || failure == ERROR_LOCK_VIOLATION) && base::TimeTicks::Now() < deadline) {
        base::PlatformThread::Sleep(base::Milliseconds(50)); continue;
      }
      base::DictValue error; error.Set("readError", true); error.Set("fileError", static_cast<int>(failure)); return error;
    }
#else
    if (!base::PathExists(file)) return std::nullopt;
    if (!base::ReadFileToStringWithMaxSize(file, &bytes, 1048576)) { base::DictValue error; error.Set("readError", true); return error; }
#endif
    auto value = base::JSONReader::Read(bytes, base::JSON_PARSE_RFC);
    if (!value || !value->is_dict()) { base::DictValue error; error.Set("readError", true); return error; }
    return std::move(*value).TakeDict();
  }
  void Configure(std::optional<base::DictValue> config) {
    if (!config || config->FindBool("readError").value_or(false)) { Fail("Recording configuration read failed"); return; }
    auto* protocol = config->FindString("protocol"); auto* capture = config->FindString("captureId"); auto* token = config->FindString("token");
    auto* output = config->FindString("outputDirectory"); auto* url = config->FindString("url"); auto* tracing = config->FindDict("traceConfig");
    if (!protocol || *protocol != kProtocol || !capture || capture->size() != 36 || !token || token->size() != 64 || !output || !url || !tracing || !config->FindBool("startPaused") || !config->FindBool("trace") || !config->FindBool("netLog")) { Fail("Recording configuration fields invalid"); return; }
    capture_id_ = *capture; token_ = *token; output_ = base::FilePath::FromUTF8Unsafe(*output).NormalizePathSeparators(); url_ = GURL(*url);
    if (!directory_.IsAbsolute() || output_ != directory_.DirName().AppendASCII("native-output") || !url_.is_valid() || !url_.SchemeIsHTTPOrHTTPS()) { Fail("Recording output directory or URL invalid"); return; }
    paused_ = *config->FindBool("startPaused"); trace_enabled_ = *config->FindBool("trace"); netlog_enabled_ = *config->FindBool("netLog");
    if (base::CommandLine::ForCurrentProcess()->HasSwitch("v8-trace-protobuf-export")) {
      auto* limit = config->FindString("maxTraceFileBytes");
      auto* format = config->FindString("traceFormat");
      auto* export_protocol = config->FindString("traceExportProtocol");
      if (!limit || !base::StringToUint64(*limit, &max_trace_file_bytes_) || !max_trace_file_bytes_ ||
          !format || *format != "perfetto-protobuf" || !export_protocol || *export_protocol != "v8trace/perfetto-file-export/2") {
        Fail("Native streaming trace format or byte bound invalid"); return;
      }
    }
    http_drain_ = config->FindBool("httpDrain").value_or(false);
    if (http_drain_ && !netlog_enabled_) { Fail("HTTP drain requires its native network witness"); return; }
    if (!base::JSONWriter::Write(*tracing, &trace_config_)) { Fail("Recording trace configuration encoding failed"); return; }
    busy_ = false; waiting_for_browser_ = true; browser_deadline_ = base::TimeTicks::Now() + base::Seconds(30);
    timer_.Start(FROM_HERE, base::Milliseconds(100), base::BindRepeating(&Controller::Poll, base::Unretained(this))); Poll();
  }
  void Poll() {
    if (busy_ || state_ == "finished" || state_ == "error") return;
    if (shutting_down_) { Finish(); return; }
    if (waiting_for_browser_) {
      auto* network = GetNetworkContext();
      if (!network) { if (base::TimeTicks::Now() > browser_deadline_) Fail("Initial browser tab unavailable"); return; }
      waiting_for_browser_ = false;
      if (netlog_enabled_) {
        network->CreateNetLogExporter(netlog_.BindNewPipeAndPassReceiver());
        netlog_.set_disconnect_handler(base::BindOnce(&Controller::Fail, base::Unretained(this), "Recording NetLog exporter disconnected"));
      }
      if (paused_) Stable(); else Begin();
      return;
    }
    if (sequence_ == std::numeric_limits<uint64_t>::max()) { Fail("Recording sequence exhausted"); return; }
    busy_ = true;
    auto file = directory_.AppendASCII("request-" + base::NumberToString(sequence_ + 1) + ".json");
    io_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&Read, file), base::BindOnce(&Controller::Request, base::Unretained(this)));
  }
  void Request(std::optional<base::DictValue> request) {
    busy_ = false;
    if (state_ == "error" || state_ == "finished") return;
    if (shutting_down_) { Finish(); return; }
    if (!request) return;
    if (request->FindBool("readError").value_or(false)) { Fail("Recording command read failed: " + base::NumberToString(request->FindInt("fileError").value_or(0))); return; }
    auto* protocol = request->FindString("protocol"); auto* capture = request->FindString("captureId"); auto* token = request->FindString("token"); auto* sequence = request->FindString("sequence"); auto* command = request->FindString("command");
    if (!protocol || *protocol != kProtocol || !capture || *capture != capture_id_ || !token || *token != token_ || !sequence || *sequence != base::NumberToString(sequence_ + 1) || !command) { Fail("Recording command identity or sequence invalid"); return; }
    ++sequence_;
    if (state_ == "draining-http" && *command != "finish") { Fail("HTTP drain accepts only the final finish command"); return; }
    if (*command == "finish") Finish();
    else if (*command == "drain") Drain();
    else if (*command == "pause") { if (paused_) Stable(); else End(false); }
    else if (*command == "resume") { if (paused_) Begin(); else Stable(); }
    else Fail("Unknown recording command");
  }
  void Begin() {
    busy_ = true; state_ = "resuming"; paused_ = false; ++window_; window_started_ = base::Time::Now().InMillisecondsSinceUnixEpoch();
    trace_file_ = trace_enabled_ ? "trace-" + base::NumberToString(window_) + (base::CommandLine::ForCurrentProcess()->HasSwitch("v8-trace-protobuf-export") ? ".pftrace" : ".json") : "";
    netlog_file_ = netlog_enabled_ ? "netlog-" + base::NumberToString(window_) + ".json" : "";
    remaining_ = int(trace_enabled_) + int(netlog_enabled_);
    if (!remaining_) { Stable(); return; }
    if (trace_enabled_) {
      auto* tracing = content::TracingController::GetInstance();
      if (tracing->IsTracing()) { Fail("Native tracing session already active"); return; }
      if (base::CommandLine::ForCurrentProcess()->HasSwitch("v8-trace-protobuf-export")) {
        io_->PostTaskAndReplyWithResult(FROM_HERE,
            base::BindOnce([](base::FilePath path) { return base::File(path, base::File::FLAG_CREATE | base::File::FLAG_WRITE); }, output_.AppendASCII(trace_file_)),
            base::BindOnce(&Controller::StartTraceFile, base::Unretained(this)));
      } else {
        if (!tracing->StartTracing(base::trace_event::TraceConfig(trace_config_), base::BindOnce(&Controller::TraceStarted, base::Unretained(this)))) { Fail("Native tracing session start rejected"); return; }
        trace_started_ = true;
      }
    }
    if (netlog_enabled_) {
      auto file = output_.AppendASCII(netlog_file_);
      io_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce([](base::FilePath path) { return base::File(path, base::File::FLAG_CREATE | base::File::FLAG_WRITE); }, file), base::BindOnce(&Controller::StartNetLog, base::Unretained(this)));
    }
  }
  void StartTraceFile(base::File file) {
    if (state_ != "resuming") return;
    if (!file.IsValid()) { Fail("Native raw trace file create failed"); return; }
    if (!content::TracingController::GetInstance()->StartTracingToFile(
            base::trace_event::TraceConfig(trace_config_), std::move(file), max_trace_file_bytes_,
            base::BindOnce(&Controller::TraceStarted, base::Unretained(this)))) {
      Fail("Native streaming trace session start rejected"); return;
    }
    trace_started_ = true;
  }
  void TraceStarted() { PartDone(0); }
  void StartNetLog(base::File file) {
    if (state_ != "resuming") return;
    if (!file.IsValid()) { Fail("Recording NetLog file create failed"); return; }
    base::DictValue constants; constants.Set("v8traceRecordingWindow", base::NumberToString(window_));
    netlog_->Start(std::move(file), std::move(constants), net::NetLogCaptureMode::kEverything, network::mojom::NetLogExporter::kUnlimitedFileSize, base::BindOnce(&Controller::NetLogStarted, base::Unretained(this)));
  }
  void NetLogStarted(int error) { if (!error) netlog_started_ = true; PartDone(error); }
  void Drain() {
    if (!http_drain_ || paused_ || state_ == "draining-http") { Fail("HTTP drain is not available in the current recording state"); return; }
    busy_ = true; state_ = "draining-http";
    if (!DrainHttp(base::BindOnce(&Controller::HttpDrained, base::Unretained(this)))) Fail("Owned capture pages were unavailable for HTTP drain");
  }
  void HttpDrained(bool committed) {
    if (state_ == "error" || state_ == "finished") return;
    if (!committed) { Fail("Owned capture navigation did not commit before HTTP drain"); return; }
    http_navigation_committed_ = true; Stable();
  }
  void End(bool finish) {
    busy_ = true; state_ = finish ? "finishing" : "pausing"; finishing_ = finish;
    remaining_ = int(trace_started_) + int(netlog_started_);
    if (!remaining_) { Ended(); return; }
    if (trace_started_) {
      auto* tracing = content::TracingController::GetInstance();
      if (base::CommandLine::ForCurrentProcess()->HasSwitch("v8-trace-protobuf-export")) {
        if (!tracing->StopTracingToFile(base::BindOnce(&Controller::FileTraceStopped, base::Unretained(this)))) { Fail("Native streaming trace stop rejected"); return; }
      } else {
        auto endpoint = content::TracingController::CreateFileEndpoint(output_.AppendASCII(trace_file_), base::BindOnce(&Controller::TraceStopped, base::Unretained(this)), base::TaskPriority::USER_BLOCKING);
        if (!tracing->StopTracing(endpoint)) { Fail("Native tracing session stop rejected"); return; }
      }
    }
    if (netlog_started_) netlog_->Stop(base::DictValue(), base::BindOnce(&Controller::PartDone, base::Unretained(this)));
  }
  void FileTraceStopped(bool success) {
    if (!success) { Fail("Native streaming trace ended early, hit its byte limit or failed file I/O/final flush"); return; }
    TraceStopped();
  }
  static bool TraceFileNonempty(const base::FilePath& path) {
    base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
    return file.IsValid() && file.GetLength() > 0;
  }
  void TraceStopped() {
    io_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&TraceFileNonempty, output_.AppendASCII(trace_file_)),
        base::BindOnce(&Controller::TraceOutputVerified, base::Unretained(this)));
  }
  void TraceOutputVerified(bool nonempty) {
    if (!nonempty) { Fail("Native trace export was empty or unreadable"); return; }
    PartDone(0);
  }
  void PartDone(int error) {
    if (state_ == "error" || state_ == "finished") return;
    if (error) { Fail("Recording NetLog operation failed: " + base::NumberToString(error)); return; }
    if (--remaining_) return;
    if (state_ == "resuming") Stable(); else Ended();
  }
  void Ended() {
    trace_started_ = netlog_started_ = false; paused_ = true;
    last_window_ = base::DictValue(); last_window_.Set("index", base::NumberToString(window_)); last_window_.Set("trace", trace_file_); last_window_.Set("netlog", netlog_file_); last_window_.Set("startedAt", base::NumberToString(window_started_)); last_window_.Set("finishedAt", base::NumberToString(base::Time::Now().InMillisecondsSinceUnixEpoch()));
    last_window_.Set("traceFormat", base::CommandLine::ForCurrentProcess()->HasSwitch("v8-trace-protobuf-export") ? "perfetto-protobuf" : "json");
    last_window_.Set("traceExportProtocol", "v8trace/perfetto-file-export/2");
    if (base::CommandLine::ForCurrentProcess()->HasSwitch("v8-trace-protobuf-export")) {
      last_window_.Set("fileStreamingProtocol", "v8trace/native-live-file/1");
      last_window_.Set("fileWritePeriodMilliseconds", 1000);
      last_window_.Set("maxTraceFileBytes", base::NumberToString(max_trace_file_bytes_));
    }
    if (finishing_ || shutting_down_) state_ = "finished";
    Stable();
  }
  void Finish() {
    if (state_ == "finished" || state_ == "error") { Quit(); return; }
    if (!paused_) End(true);
    else { state_ = "finished"; Stable(); }
  }
  void Stable() {
    if (state_ != "finished" && state_ != "draining-http") state_ = paused_ ? "paused" : "recording";
    busy_ = true;
    WriteStatus(base::BindOnce(&Controller::Written, base::Unretained(this)));
  }
  void Written(bool success) {
    busy_ = false;
    if (!success) { LOG(ERROR) << "V8Trace recording status write failed"; state_ = "error"; timer_.Stop(); Quit(); return; }
    if (state_ == "finished" || state_ == "error") { timer_.Stop(); Quit(); return; }
    if (shutting_down_) { Finish(); return; }
    if (!navigated_) {
      navigated_ = true;
      if (!Navigate(url_)) { Fail("Initial browser tab disappeared"); return; }
    }
  }
  void WriteStatus(base::OnceCallback<void(bool)> done) {
    base::DictValue status;
    status.Set("protocol", kProtocol); status.Set("captureId", capture_id_); status.Set("token", token_);
    status.Set("browserPid", static_cast<double>(base::GetCurrentProcId())); status.Set("sequence", base::NumberToString(sequence_));
    status.Set("state", state_); status.Set("paused", paused_);
    if (http_drain_) {
      base::DictValue drain; drain.Set("protocol", "v8trace/http-drain/1"); drain.Set("navigationCommitted", http_navigation_committed_);
      status.Set("httpDrain", std::move(drain));
    }
    if (!last_window_.empty()) status.Set("lastWindow", last_window_.Clone());
    if (!error_.empty()) status.Set("error", error_);
    std::string bytes;
    if (!base::JSONWriter::Write(status, &bytes)) { std::move(done).Run(false); return; }
    io_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce([](base::FilePath file, std::string value) { return base::ImportantFileWriter::WriteFileAtomically(file, value, "V8TraceRecording"); }, directory_.AppendASCII("status.json"), std::move(bytes)), std::move(done));
  }
  void Fail(std::string error) {
    if (state_ == "error") { Quit(); return; }
    error_ = std::move(error); state_ = "error"; timer_.Stop(); LOG(ERROR) << "V8Trace: " << error_;
    if (io_) WriteStatus(base::BindOnce(&Controller::Written, base::Unretained(this))); else Quit();
  }
  void Quit() { if (finished_) std::move(finished_).Run(); }

  bool started_ = false, busy_ = false, paused_ = true, waiting_for_browser_ = false, navigated_ = false, shutting_down_ = false;
  bool trace_enabled_ = false, netlog_enabled_ = false, trace_started_ = false, netlog_started_ = false, finishing_ = false;
  bool http_drain_ = false, http_navigation_committed_ = false;
  int remaining_ = 0; uint64_t sequence_ = 0, window_ = 0; int64_t window_started_ = 0;
  uint64_t max_trace_file_bytes_ = 0;
  std::string capture_id_, token_, trace_config_, state_ = "starting", error_, trace_file_, netlog_file_;
  base::FilePath config_file_, directory_, output_; GURL url_; base::DictValue last_window_;
  base::RepeatingTimer timer_; base::TimeTicks browser_deadline_; base::OnceClosure finished_;
  scoped_refptr<base::SequencedTaskRunner> io_; mojo::Remote<network::mojom::NetLogExporter> netlog_;
};
inline Controller::Controller() = default;
inline Controller::~Controller() = default;
inline void Start() { Controller::Get().Start(); }
inline void Shutdown() { Controller::Get().Shutdown(); }
}
#endif
