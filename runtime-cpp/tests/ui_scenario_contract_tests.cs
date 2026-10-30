using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using V8Trace;

public static class ScenarioContractTests {
    static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 16777216 };
    static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);
    static int checks;
    static string package, fixture, evidence;
    static void Check(bool value, string message) { ++checks; if (!value) throw new Exception(message); }
    static object Invoke(TraceWindow form, string name, params object[] args) { return typeof(TraceWindow).GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic).Invoke(form, args); }
    static FieldInfo Field(string name) { return typeof(TraceWindow).GetField(name, BindingFlags.Instance | BindingFlags.NonPublic); }
    static T Find<T>(Control parent, string name) where T : Control { return (T)parent.Controls.Find(name, true).Single(); }
    static Dictionary<string, object> Obj(object value) { return (Dictionary<string, object>)value; }
    static Dictionary<string, object> Read(string file) { return Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(file, Utf8)); }
    static void Write(string file, object value) { File.WriteAllText(file, Json.Serialize(value), Utf8); }
    static string Hash(string file) { using (var hash = SHA256.Create()) using (var stream = File.OpenRead(file)) return BitConverter.ToString(hash.ComputeHash(stream)).Replace("-", "").ToLowerInvariant(); }
    static void Wait(Func<bool> done, string message) { var until = DateTime.UtcNow.AddSeconds(45); while (!done()) { if (DateTime.UtcNow > until) throw new Exception("Timeout: " + message); Application.DoEvents(); Thread.Sleep(10); } Application.DoEvents(); }
    static string Quote(string value) { return "\"" + value.Replace("\"", "\\\"") + "\""; }
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    static void Snapshot(Form form, string name) {
        Application.DoEvents(); form.Refresh();
        using (var bitmap = new Bitmap(form.Width, form.Height)) using (var graphics = Graphics.FromImage(bitmap)) {
            var dc = graphics.GetHdc(); try { Check(PrintWindow(form.Handle, dc, 2), "actual diagnostic panel render"); } finally { graphics.ReleaseHdc(dc); }
            bitmap.Save(Path.Combine(evidence, name), System.Drawing.Imaging.ImageFormat.Png);
        }
    }
    static Dictionary<string, object> Plan(object options, string name, bool accepted) {
        var config = Path.Combine(fixture, name + ".json"); Write(config, options);
        var start = new ProcessStartInfo(Path.Combine(package, "runtime", "v8trace-runtime.exe"), "preflight --config " + Quote(config) + " --assets " + Quote(Path.Combine(package, "app", "v8-trace"))) {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
        };
        using (var process = Process.Start(start)) {
            var output = process.StandardOutput.ReadToEndAsync(); var error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(45000)) { process.Kill(); throw new Exception("Own fixture CLI timeout"); }
            File.WriteAllText(Path.Combine(evidence, name + ".json"), output.Result, Utf8);
            File.WriteAllText(Path.Combine(evidence, name + ".stderr.log"), error.Result, Utf8);
            var result = Json.Deserialize<Dictionary<string, object>>(output.Result);
            Check(process.ExitCode == (accepted ? 0 : 2) && Convert.ToBoolean(result["ok"]) == accepted, name + ": explicit CLI outcome " + output.Result);
            return result;
        }
    }
    [STAThread] public static int Main(string[] args) {
        package = Path.GetFullPath(args[0]); evidence = Path.GetFullPath(args[1]); fixture = Path.GetFullPath(args[2]);
        var edition = args[3]; TraceWindow form = null; bool success = false; string failure = "";
        try {
            var browser = Path.Combine(fixture, "browser"); Directory.CreateDirectory(browser);
            var exe = Path.Combine(browser, "chrome.exe"); var dll = Path.Combine(browser, "chrome.dll");
            File.WriteAllText(exe, "fixture-not-executable", Utf8); File.WriteAllText(dll, "fixture-not-an-engine", Utf8);
            var descriptor = Read(args[4]); descriptor["edition"] = edition;
            descriptor["binaryHashes"] = new Dictionary<string, object> { { "chrome.exe", Hash(exe) }, { "chrome.dll", Hash(dll) } };
            Write(Path.Combine(browser, "v8trace-opcode.json"), descriptor);
            Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            form = new TraceWindow(Path.Combine(fixture, "theme.json")) { ShowInTaskbar = false, Opacity = 0 };
            form.Show(); Wait(() => (bool)Field("ready").GetValue(form), "actual worker ready");
            var selector = Find<ComboBox>(form, "PresetInput"); Check(selector.Items.Count == 22, "21 presets and manual choice");
            var scope = Find<TextBox>(form, "PresetScriptScopeInput"); scope.Text = "fixture.js";
            Find<CheckBox>(form, "HeadlessOption").Checked = true;
            Find<CheckBox>(form, "RecordingControlOption").Checked = true;
            Find<CheckBox>(form, "StartPausedOption").Checked = true;
            Find<TextBox>(form, "UrlInput").Text = "https://fixture.test/";
            Find<TextBox>(form, "OutputInput").Text = Path.Combine(fixture, "must-not-create");
            for (int i = 1; i < selector.Items.Count; ++i) {
                selector.SelectedIndex = i;
                var record = Obj(selector.SelectedItem.GetType().GetField("Record").GetValue(selector.SelectedItem));
                var id = Convert.ToString(record["id"]); var before = Json.Serialize(Invoke(form, "CaptureOptions"));
                Invoke(form, "ApplyPreset");
                if (!Convert.ToBoolean(record["available"])) {
                    Check(Json.Serialize(Invoke(form, "CaptureOptions")) == before, "restricted preset leaves configuration untouched: " + id);
                    continue;
                }
                Find<TextBox>(form, "ChromeInput").Text = exe;
                var actual = Obj(Invoke(form, "CaptureOptions"));
                var expected = new Dictionary<string, object> { { "preset", id }, { "url", actual["url"] }, { "output", actual["output"] }, { "chromePath", exe },
                    { "headless", true }, { "recordingControl", true }, { "startPaused", true } };
                Check(Convert.ToBoolean(actual["headless"]) && Convert.ToBoolean(actual["recordingControl"]) && Convert.ToBoolean(actual["startPaused"]), "explicit runtime selections preserved " + id);
                if (Convert.ToBoolean(record["requiresScriptScope"])) expected["detailScriptInclude"] = "fixture.js";
                var direct = Obj(Plan(expected, id + "-cli", true)["plan"]); var ui = Obj(Plan(actual, id + "-ui", true)["plan"]);
                foreach (var key in Obj(record["options"]).Keys)
                    Check(Json.Serialize(direct[key]) == Json.Serialize(ui[key]), "actual UI/CLI normalized option " + id + " / " + key);
                Check(Json.Serialize(direct["scenarioContract"]) == Json.Serialize(ui["scenarioContract"]), "identical effective UI/CLI contract " + id);
                Check(Find<RichTextBox>(form, "PresetHint").Text.Contains("v8trace/scenario-contract/1"), "UI consumes worker contract " + id);
            }
            var exact = new Dictionary<string, object> { { "preset", "env-protocol" }, { "url", "https://fixture.test/" },
                { "output", Path.Combine(fixture, "exact-not-created") }, { "chromePath", exe }, { "maxBytes", 123456789 }, { "unknownFullValue", "原始😀" } };
            var normalized = Obj(Plan(exact, "exact-cli", true)["plan"]);
            Invoke(form, "ApplyConfigurationDocument", exact, normalized);
            var roundtrip = Obj(Invoke(form, "CaptureOptions"));
            Check(Convert.ToInt64(roundtrip["maxBytes"]) == 123456789 && Convert.ToString(roundtrip["unknownFullValue"]) == "原始😀", "UI preserves exact imported budget and unknown values");
            descriptor.Remove("redirectBodyProtocol"); Write(Path.Combine(browser, "v8trace-opcode.json"), descriptor);
            var missing = Plan(roundtrip, "missing-capability", false);
            Check(Convert.ToString(missing["error"]).Contains("Redirect body protocol missing") && !Convert.ToBoolean(missing["browserVerified"]), "missing capability is explicit and not verified");
            var capture = Path.Combine(fixture, "capture"); Directory.CreateDirectory(capture);
            var finding = new Dictionary<string, object> { { "gapSeq", 1 }, { "reason", "capture-byte-budget" }, { "source", "unknown" }, { "stage", "unknown" }, { "suggestedOptions", new Dictionary<string, object> { { "maxBytes", 246913578 } } } };
            var summary = new Dictionary<string, object> { { "captureConclusion", "failed" }, { "integrityConclusion", "partial" }, { "criticalGapCount", 1 },
                { "diagnostics", new Dictionary<string, object> { { "findings", new object[] { finding } } } } };
            Write(Path.Combine(capture, "summary.json"), summary);
            File.WriteAllText(Path.Combine(capture, "gaps.jsonl"), Json.Serialize(new { gapSeq = 1, reason = "capture-byte-budget", critical = true, details = new { raw = "完整值" }, diagnostic = finding }) + "\n", Utf8);
            var rawHash = Hash(Path.Combine(capture, "gaps.jsonl")); Field("captureDirectory").SetValue(form, capture);
            var configurationBefore = Json.Serialize(Invoke(form, "CaptureOptions")); Invoke(form, "ShowSummary", summary);
            Find<TabControl>(form, "EvidenceTabs").SelectedIndex = 1; Snapshot(form, "diagnostics-1280.png");
            var patch = Obj(Invoke(form, "RecaptureOptionsDocument"));
            Check(Convert.ToInt64(Obj(patch["options"])["maxBytes"]) == 246913578 && !Obj(patch["options"]).ContainsKey("output"), "explicit merge patch never overwrites old output");
            Check(Json.Serialize(Invoke(form, "CaptureOptions")) == configurationBefore, "viewing/copying diagnostics never applies suggestions");
            Check(Find<Label>(form, "PhaseLabel").Text.Contains("失败"), "failed capture stays failed in UI");
            Invoke(form, "OpenGapEvidence"); Wait(() => !(bool)Field("busy").GetValue(form) && Find<DataGridView>(form, "IndexGrid").Rows.Count == 1, "full raw gap index and first page");
            Check(Find<ComboBox>(form, "IndexCategoryInput").Text == "gaps" && Hash(Path.Combine(capture, "gaps.jsonl")) == rawHash, "gap navigation is read-only");
            Check(!Directory.Exists(Path.Combine(fixture, "must-not-create")) && !Directory.Exists(Path.Combine(fixture, "exact-not-created")), "no fixture browser or capture launched");
            form.Close(); Wait(() => form.IsDisposed, "owned worker shutdown"); success = true;
        } catch (Exception error) { failure = error.ToString(); Console.Error.WriteLine(failure); }
        finally {
            if (form != null && !form.IsDisposed) { try { Invoke(form, "SetBusy", false); form.Close(); Wait(() => form.IsDisposed, "failure shutdown"); } catch { form.Dispose(); } }
        }
        Write(Path.Combine(evidence, "result.json"), new { protocol = "v8trace/ui-scenario-contract-test/1", edition = edition, status = success ? "passed" : "failed", checks = checks, error = failure,
            browserLaunched = false, userPreferencesTouched = false, clipboardTouched = false, uiSha256 = Hash(Path.Combine(package, "V8Trace.exe")), runtimeSha256 = Hash(Path.Combine(package, "runtime", "v8trace-runtime.exe")) });
        return success ? 0 : 1;
    }
}
