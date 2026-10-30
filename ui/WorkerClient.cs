using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;

namespace V8Trace {
    public sealed class WorkerClient : IDisposable {
        readonly ConcurrentQueue<string> messages = new ConcurrentQueue<string>();
        readonly object queueGate = new object();
        readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = 8 * 1024 * 1024 };
        Process process;
        StreamWriter input;
        int sequence;
        long queuedMessages, queuedUtf16Bytes, enqueuedMessages, dequeuedMessages, highWaterMessages, highWaterUtf16Bytes;
        bool started, stdoutEnded = true, stderrEnded = true;
        public bool Alive { get { return started && process != null && !process.HasExited; } }
        public bool OutputDrained { get { lock (queueGate) return !Alive && stdoutEnded && stderrEnded && queuedMessages == 0; } }
        public Dictionary<string, object> QueueMetrics { get { lock (queueGate) return new Dictionary<string, object> {
            { "protocol", "v8trace/ui-queue/1" }, { "pendingMessages", queuedMessages }, { "pendingUtf16Bytes", queuedUtf16Bytes },
            { "enqueuedMessages", enqueuedMessages }, { "dequeuedMessages", dequeuedMessages },
            { "highWaterMessages", highWaterMessages }, { "highWaterUtf16Bytes", highWaterUtf16Bytes },
            { "stdoutEnded", stdoutEnded }, { "stderrEnded", stderrEnded },
            { "byteMeaning", "queued UTF-16 string payload; excludes object and parser overhead" },
            { "discardPolicy", "none; every queued wire line is read in arrival order" } }; } }
        void Enqueue(string text) {
            lock (queueGate) {
                messages.Enqueue(text); ++queuedMessages; ++enqueuedMessages; queuedUtf16Bytes += (long)text.Length * 2;
                highWaterMessages = Math.Max(highWaterMessages, queuedMessages); highWaterUtf16Bytes = Math.Max(highWaterUtf16Bytes, queuedUtf16Bytes);
            }
        }

        public void Start(string root) {
            var runtime = Path.Combine(root, "runtime", "v8trace-runtime.exe");
            if (!File.Exists(runtime)) throw new FileNotFoundException("请保留完整便携软件目录，C++ 采集内核缺失；不会自动回退其他运行时。");
            process = new Process { StartInfo = new ProcessStartInfo {
                FileName = runtime, Arguments = "--worker", WorkingDirectory = root,
                UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden,
                RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
            }};
            process.OutputDataReceived += (sender, args) => { if (args.Data == null) { lock (queueGate) stdoutEnded = true; } else Enqueue(args.Data); };
            process.ErrorDataReceived += (sender, args) => { if (args.Data == null) { lock (queueGate) stderrEnded = true; } else if (args.Data.Length > 0) Enqueue(json.Serialize(new { type = "worker-stderr", error = args.Data })); };
            process.Start();
            started = true;
            try {
                input = new StreamWriter(process.StandardInput.BaseStream, new UTF8Encoding(false)) { AutoFlush = true };
                lock (queueGate) stdoutEnded = false;
                try { process.BeginOutputReadLine(); } catch { lock (queueGate) stdoutEnded = true; throw; }
                lock (queueGate) stderrEnded = false;
                try { process.BeginErrorReadLine(); } catch { lock (queueGate) stderrEnded = true; throw; }
            } catch {
                if (input != null) { input.Dispose(); input = null; } else process.StandardInput.Close();
                throw;
            }
        }

        public string Send(string command, Dictionary<string, object> fields = null) {
            if (!Alive) throw new InvalidOperationException("采集内核尚未就绪或已经退出。");
            var message = fields ?? new Dictionary<string, object>();
            var id = (++sequence).ToString();
            message["id"] = id;
            message["command"] = command;
            input.WriteLine(json.Serialize(message));
            return id;
        }

        public bool TryRead(out Dictionary<string, object> message) {
            string text;
            lock (queueGate) {
                if (!messages.TryDequeue(out text)) { message = null; return false; }
                --queuedMessages; ++dequeuedMessages; queuedUtf16Bytes -= (long)text.Length * 2;
            }
            try { message = json.Deserialize<Dictionary<string, object>>(text); }
            catch (Exception error) { message = new Dictionary<string, object> { { "type", "error" }, { "error", "界面协议解析失败：" + error.Message } }; }
            return true;
        }

        public void Dispose() {
            if (input != null) { input.Dispose(); input = null; }
            if (process != null) { process.Dispose(); process = null; started = false; }
        }
    }
}
