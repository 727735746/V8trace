using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using V8Trace;

public static class QueueBenchmark {
    static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 16 * 1024 * 1024 };
    static int checks;
    static FieldInfo Field(string name) { return typeof(TraceWindow).GetField(name, BindingFlags.NonPublic | BindingFlags.Instance); }
    static object Invoke(TraceWindow form, string name, params object[] values) { return typeof(TraceWindow).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Instance).Invoke(form, values); }
    static T Find<T>(Control form, string name) where T : Control { return (T)form.Controls.Find(name, true).Single(); }
    static void Check(bool value, string message) { ++checks; if (!value) throw new Exception(message); }
    static void Wait(Func<bool> done, string message) { var until = DateTime.UtcNow.AddSeconds(90); while (!done()) { if (DateTime.UtcNow > until) throw new Exception("Timeout: " + message); Application.DoEvents(); Thread.Sleep(5); } Application.DoEvents(); }
    static string[] Pending(WorkerClient worker) { return ((IEnumerable)typeof(WorkerClient).GetField("messages", BindingFlags.NonPublic | BindingFlags.Instance).GetValue(worker)).Cast<string>().ToArray(); }
    static bool Has(WorkerClient worker, string type) { return Pending(worker).Any(line => line.Substring(0, Math.Min(line.Length, 120)).Contains("\"" + type + "\"")); }
    static string Hash(string file) { using (var hash = SHA256.Create()) using (var stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) return BitConverter.ToString(hash.ComputeHash(stream)).Replace("-", "").ToLowerInvariant(); }
    static double Cpu() { using (var process = Process.GetCurrentProcess()) return process.TotalProcessorTime.TotalMilliseconds; }
    static object Metrics(WorkerClient worker) {
        var property = typeof(WorkerClient).GetProperty("QueueMetrics");
        if (property != null) return property.GetValue(worker, null);
        var rows = Pending(worker); return new { pendingMessages = rows.Length, pendingUtf16Bytes = rows.Sum(line => (long)line.Length * 2), scope = "baseline quiescent queue snapshot" };
    }
    [STAThread] public static int Main(string[] args) {
        string evidence = args[0], fixture = args[1], edition = args[2]; bool strictExit = args.Length > 3 && args[3] == "drained"; TraceWindow form = null; bool success = false; string error = ""; object result = null;
        try {
            Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            form = new TraceWindow(Path.Combine(fixture, "theme.json")) { ShowInTaskbar = false, Opacity = 0 };
            form.Show(); Wait(() => (bool)Field("ready").GetValue(form), "fixture protocol ready");
            ((System.Windows.Forms.Timer)Field("timer").GetValue(form)).Stop();
            var worker = (WorkerClient)Field("worker").GetValue(form);
            worker.Send("fixture-load"); Wait(() => Has(worker, "fixture-end"), "transport workload prefilled");
            var transportPeak = Metrics(worker); var transport = new List<int>(); Dictionary<string, object> message; bool giant = false;
            while (worker.TryRead(out message)) {
                if (message.ContainsKey("fixtureOrder")) transport.Add(Convert.ToInt32(message["fixtureOrder"]));
                if (Convert.ToString(message["type"]) == "fixture-large") { var value = Convert.ToString(message["fullValue"]); giant = value.Length == 3 * 1024 * 1024 + 1 && value.EndsWith("中"); }
            }
            Check(giant && transport.Count == 127 && transport.SequenceEqual(Enumerable.Range(1, 127)), "all transport/control messages, order and giant value retained");
            var grid = Find<DataGridView>(form, "EventsGrid"); var pause = Find<Button>(form, "PauseRecordingButton"); var phase = Find<Label>(form, "PhaseLabel");
            var clock = new Stopwatch(); var acknowledgments = new List<double>(); double stopReply = -1;
            pause.TextChanged += delegate { if (clock.IsRunning) acknowledgments.Add(clock.Elapsed.TotalMilliseconds); };
            phase.TextChanged += delegate { if (clock.IsRunning && phase.Text.Contains("停止请求已发送")) stopReply = clock.Elapsed.TotalMilliseconds; };
            Invoke(form, "SetBusy", true); var id = worker.Send("fixture-load"); Field("captureRequestId").SetValue(form, id);
            Wait(() => Has(worker, "fixture-end"), "UI workload prefilled"); worker.Send("stop"); Wait(() => Has(worker, "fixture-stop-ack"), "critical stop reply enqueued");
            var before = Metrics(worker); var queued = Pending(worker).Length; long rowsBuilt = 0; int pumps = 0;
            DataGridViewRowsAddedEventHandler rowCounter = (sender, change) => rowsBuilt += change.RowCount; grid.RowsAdded += rowCounter;
            var wire = Path.Combine(fixture, "runtime", "fixture-wire.jsonl"); var wireHash = Hash(wire); var cpu = Cpu(); clock.Start();
            while (Pending(worker).Length > 0) { Invoke(form, "Pump"); ++pumps; }
            clock.Stop(); var cpuMilliseconds = Cpu() - cpu; grid.RowsAdded -= rowCounter;
            Check(grid.Rows.Count == 1000 && ((ICollection)Field("previews").GetValue(form)).Count == 1000, "existing 1000-preview boundary");
            var final = grid.Rows[999].Tag as Dictionary<string, object>;
            Check(Convert.ToInt32(final["seq"]) == 1200 && acknowledgments.Count == 4 && Convert.ToInt64(Field("recordingGeneration").GetValue(form)) == 3, "final preview and ordered recording replies; stale reply rejected");
            Check(stopReply >= 0 && Hash(wire) == wireHash, "critical stop reply consumed; all original wire evidence unchanged");
            var peakAfterDrain = Metrics(worker);
            var chosen = grid.Rows.Cast<DataGridViewRow>().Single(row => Convert.ToInt32(((Dictionary<string, object>)row.Tag)["seq"]) == 400);
            grid.ClearSelection(); chosen.Selected = true;
            worker.Send("fixture-increment"); Wait(() => Has(worker, "fixture-end"), "incremental presentation workload");
            while (Pending(worker).Length > 0) Invoke(form, "Pump");
            Check(Convert.ToInt32(((Dictionary<string, object>)grid.SelectedRows[0].Tag)["seq"]) == 400, "selected-row identity survives appended batch");
            var filter = Find<TextBox>(form, "TextFilter"); filter.Text = "needle";
            Check(grid.Rows.Count == 10 && ((ICollection)Field("previews").GetValue(form)).Count == 1000, "display filtering does not drop previews");
            filter.Clear();
            worker.Send("fixture-invalid"); Wait(() => Pending(worker).Length > 0, "malformed fixture line");
            Check(worker.TryRead(out message) && Convert.ToString(message["type"]) == "error", "parse error semantics preserved");
            Invoke(form, "SetBusy", false); Field("captureRequestId").SetValue(form, null);
            var archives = Path.Combine(Path.GetDirectoryName(fixture), "archive-fixtures"); Directory.CreateDirectory(archives);
            for (int n = 0; n < 3000; ++n) File.WriteAllText(Path.Combine(archives, "fixture-" + n.ToString("D5") + ".jsonl"), "{\"fixture\":true}\n", new UTF8Encoding(false));
            var listing = Stopwatch.StartNew(); Find<TextBox>(form, "LogRootInput").Text = archives; Invoke(form, "RefreshLogs");
            Wait(() => !(bool)Field("logsScanning").GetValue(form), "large owned archive listing"); listing.Stop();
            Check(Find<DataGridView>(form, "LogTable").Rows.Count == 3000, "large listing represented without deleting files");
            Invoke(form, "RefreshLogs"); var cancel = Stopwatch.StartNew(); Invoke(form, "CancelLogScan");
            Wait(() => !(bool)Field("logsScanning").GetValue(form), "archive cancellation"); cancel.Stop();
            Check(Directory.GetFiles(archives).Length == 3000, "cancel keeps every owned archive fixture");
            var storage = (LogStorage)Field("logStorage").GetValue(form);
            double workerCancellation;
            using (var cancellation = new CancellationTokenSource()) using (var entered = new ManualResetEvent(false)) {
                var scan = Task.Factory.StartNew(delegate { entered.Set(); try { storage.Scan(archives, new string[0], cancellation.Token); } catch (OperationCanceledException) { } });
                entered.WaitOne(); var stopScan = Stopwatch.StartNew(); cancellation.Cancel();
                Check(scan.Wait(15000), "owned scan worker actually terminates after cancellation"); stopScan.Stop(); workerCancellation = stopScan.Elapsed.TotalMilliseconds;
            }
            bool exitMarkerObserved = false;
            var summary = Find<RichTextBox>(form, "SummaryText");
            summary.TextChanged += delegate { if (summary.Text.Contains("fixture-final-exit-marker")) exitMarkerObserved = true; };
            ((System.Windows.Forms.Timer)Field("timer").GetValue(form)).Start();
            var close = Stopwatch.StartNew(); form.Close(); Wait(() => form.IsDisposed, "fixture worker shutdown"); close.Stop();
            var pendingOnClose = Pending(worker).Length; var queueAfterExit = Metrics(worker);
            if (strictExit) {
                Check(exitMarkerObserved && pendingOnClose == 0, "worker exit waits for both EOFs and every queued control reply");
                var snapshot = queueAfterExit as Dictionary<string, object>;
                Check(snapshot != null && Convert.ToInt64(snapshot["enqueuedMessages"]) == 362 && Convert.ToInt64(snapshot["dequeuedMessages"]) == 362
                    && Convert.ToInt64(snapshot["pendingUtf16Bytes"]) == 0 && Convert.ToBoolean(snapshot["stdoutEnded"]) && Convert.ToBoolean(snapshot["stderrEnded"]), "exact quiescent queue/EOF accounting");
            }
            File.Copy(wire, Path.Combine(evidence, "fixture-wire.jsonl"), false);
            result = new { protocol = "v8trace/ui-queue-benchmark/1", edition = edition, producer = "owned protocol fixture; not browser capture",
                transportMessages = transport.Count, giantValueRetained = giant, transportPeak = transportPeak, queuedBeforeDrain = queued, queueBeforeDrain = before, queueAfterDrain = peakAfterDrain,
                pumpCalls = pumps, rowsBuilt = rowsBuilt, wallMilliseconds = clock.Elapsed.TotalMilliseconds, cpuMilliseconds = cpuMilliseconds, acknowledgmentMilliseconds = acknowledgments,
                stopReplyMilliseconds = stopReply, shutdownMilliseconds = close.Elapsed.TotalMilliseconds, archiveEntries = 3000, archiveListingMilliseconds = listing.Elapsed.TotalMilliseconds,
                archiveCancelAcknowledgmentMilliseconds = cancel.Elapsed.TotalMilliseconds, archiveWorkerCancelMilliseconds = workerCancellation,
                sourceWireSha256 = wireHash, sourceWireUnchanged = true, finalWireSha256 = Hash(wire),
                pendingOnClose = pendingOnClose, exitMarkerObserved = exitMarkerObserved, queueAfterExit = queueAfterExit, transportSequenceRetained = true,
                drainMode = "prefilled burst; actual 80-message Pump; offscreen form; excludes normal 200ms timer waits",
                wireIntegrityScope = "unchanged during measured drain; later owned fixture records appended by producer" };
            success = true;
        } catch (Exception failure) { error = failure.ToString(); Console.Error.WriteLine(error); }
        finally { if (form != null && !form.IsDisposed) { try { ((System.Windows.Forms.Timer)Field("timer").GetValue(form)).Start(); Invoke(form, "SetBusy", false); form.Close(); Wait(() => form.IsDisposed, "failure shutdown"); } catch { form.Dispose(); } } }
        File.WriteAllText(Path.Combine(evidence, "result.json"), Json.Serialize(new { status = success ? (strictExit ? "passed" : "baseline-measured") : "failed", checks = checks, error = error, measurements = result,
            uiSha256 = Hash(typeof(TraceWindow).Assembly.Location), browserLaunched = false, userPreferencesTouched = false, userLogsTouched = false }), new UTF8Encoding(false));
        return success ? 0 : 1;
    }
}
