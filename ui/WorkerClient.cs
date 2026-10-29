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
        readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = 8 * 1024 * 1024 };
        Process process;
        StreamWriter input;
        int sequence;
        public bool Alive { get { return process != null && !process.HasExited; } }

        public void Start(string root) {
            var runtime = Path.Combine(root, "runtime", "v8trace-runtime.exe");
            if (!File.Exists(runtime)) throw new FileNotFoundException("请保留完整便携软件目录，C++ 采集内核缺失；不会自动回退其他运行时。");
            process = new Process { StartInfo = new ProcessStartInfo {
                FileName = runtime, Arguments = "--worker", WorkingDirectory = root,
                UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden,
                RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
            }};
            process.OutputDataReceived += (sender, args) => { if (args.Data != null) messages.Enqueue(args.Data); };
            process.ErrorDataReceived += (sender, args) => { if (!String.IsNullOrEmpty(args.Data)) messages.Enqueue(json.Serialize(new { type = "worker-stderr", error = args.Data })); };
            process.Start();
            input = new StreamWriter(process.StandardInput.BaseStream, new UTF8Encoding(false)) { AutoFlush = true };
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
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
            if (!messages.TryDequeue(out text)) { message = null; return false; }
            try { message = json.Deserialize<Dictionary<string, object>>(text); }
            catch (Exception error) { message = new Dictionary<string, object> { { "type", "error" }, { "error", "界面协议解析失败：" + error.Message } }; }
            return true;
        }

        public void Dispose() {
            if (input != null) { input.Dispose(); input = null; }
            if (process != null) { process.Dispose(); process = null; }
        }
    }
}
