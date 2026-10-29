using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading.Tasks;

namespace V8Trace {
    public sealed class McpService : IDisposable {
        readonly object gate = new object();
        readonly ConcurrentQueue<Tuple<int, string>> messages = new ConcurrentQueue<Tuple<int, string>>();
        Process process;
        Task stopping;
        int generation;
        string bearer, address;
        public bool Active { get { lock (gate) return process != null || stopping != null; } }

        public static string Token(string file) {
            if (!File.Exists(file)) {
                Directory.CreateDirectory(Path.GetDirectoryName(file));
                var security = new FileSecurity(); security.SetAccessRuleProtection(true, false);
                var user = WindowsIdentity.GetCurrent().User; security.SetOwner(user);
                security.AddAccessRule(new FileSystemAccessRule(user, FileSystemRights.FullControl, AccessControlType.Allow));
                security.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null), FileSystemRights.FullControl, AccessControlType.Allow));
                var bytes = new byte[32]; using (var random = RandomNumberGenerator.Create()) random.GetBytes(bytes);
                var encoded = Encoding.ASCII.GetBytes(BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant() + "\n");
                using (var stream = new FileStream(file, FileMode.CreateNew, FileSystemRights.Write, FileShare.None, 4096, FileOptions.None, security)) { stream.Write(encoded, 0, encoded.Length); stream.Flush(true); }
            }
            if (new FileInfo(file).Length > 4096) throw new InvalidOperationException("令牌文件超过 4 KiB。");
            var token = File.ReadAllText(file, new UTF8Encoding(false, true)).Trim();
            if (token.Length < 24 || token.Length > 256 || token.Any(c => c < 33 || c > 126)) throw new InvalidOperationException("令牌须为 24 至 256 个可打印 ASCII 字节。");
            return token;
        }
        public static string Quote(string value) {
            var result = new StringBuilder("\""); int slashes = 0;
            foreach (char c in value) {
                if (c == '\\') { slashes++; continue; }
                result.Append('\\', c == '"' ? slashes * 2 + 1 : slashes); result.Append(c); slashes = 0;
            }
            return result.Append('\\', slashes * 2).Append('"').ToString();
        }
        void Enqueue(int id, string line) { if (line != null && messages.Count < 64) messages.Enqueue(Tuple.Create(id, line)); }
        public void Start(string root, string source, string index, string exports, string tokenFile, int port) {
            lock (gate) {
                if (process != null || stopping != null) throw new InvalidOperationException("当前 MCP 服务尚未结束。");
                if (!Directory.Exists(source)) throw new DirectoryNotFoundException("请选择已有的日志根目录。");
                bearer = Token(tokenFile); address = "http://127.0.0.1:" + port; int id = ++generation;
                string runtime = Path.Combine(root, "runtime", "v8trace-runtime.exe");
                var args = new[] { "mcp", "--transport", "http", "--bind", "127.0.0.1", "--port", port.ToString(), "--source-root", source, "--index-dir", index, "--export-root", exports, "--token-file", tokenFile };
                var owned = new Process { StartInfo = new ProcessStartInfo { FileName = runtime, Arguments = String.Join(" ", args.Select(Quote)), WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden, RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8 } };
                owned.OutputDataReceived += (sender, e) => Enqueue(id, e.Data); owned.ErrorDataReceived += (sender, e) => Enqueue(id, e.Data);
                try { owned.Start(); process = owned; owned.BeginOutputReadLine(); owned.BeginErrorReadLine(); }
                catch { if (process == owned) process = null; owned.Dispose(); throw; }
            }
        }
        public void Poll() {
            lock (gate) {
                if (stopping != null && stopping.IsCompleted) { stopping.Dispose(); stopping = null; }
                if (process != null && stopping == null && process.HasExited) {
                    Enqueue(generation, "{\"type\":\"mcp-exited\",\"code\":" + process.ExitCode + "}"); process.Dispose(); process = null;
                }
            }
        }
        public bool TryRead(out string line) {
            Tuple<int, string> item;
            while (messages.TryDequeue(out item)) if (item.Item1 == generation) { line = item.Item2; return true; }
            line = null; return false;
        }
        public void RequestStop() {
            lock (gate) {
                if (process == null || stopping != null) return;
                var owned = process; var endpoint = address; var token = bearer; int id = generation;
                stopping = Task.Factory.StartNew(() => {
                    bool forced = false;
                    try {
                        if (!owned.HasExited) {
                            try { var request = (HttpWebRequest)WebRequest.Create(endpoint + "/shutdown"); request.Proxy = null; request.Method = "POST"; request.ContentLength = 0; request.Timeout = 1500; request.Headers["Authorization"] = "Bearer " + token; using (var response = request.GetResponse()) {} } catch (WebException) {}
                            if (!owned.WaitForExit(10000)) { forced = true; owned.Kill(); owned.WaitForExit(3000); }
                        }
                        Enqueue(id, "{\"type\":\"mcp-stopped\",\"forced\":" + (forced ? "true" : "false") + "}");
                    } catch (Exception error) { Enqueue(id, "MCP 停止错误：" + error.Message); }
                    finally { lock (gate) { if (process == owned) { owned.Dispose(); process = null; bearer = null; } } }
                });
            }
        }
        public void Dispose() {
            RequestStop(); Task pending; lock (gate) pending = stopping;
            if (pending != null) pending.Wait(15000);
            Poll();
        }
    }
}
