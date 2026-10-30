using System;
using System.Collections.Generic;
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

public static class GuidedWorkflowTests {
    static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 16777216 };
    static readonly UTF8Encoding Utf8 = new UTF8Encoding(false);
    static int checks;
    static string evidence, fixture, chrome;
    static FieldInfo Field(string name) { return typeof(TraceWindow).GetField(name, BindingFlags.NonPublic | BindingFlags.Instance); }
    static object Invoke(TraceWindow form, string name, params object[] values) { return typeof(TraceWindow).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Instance).Invoke(form, values); }
    static T Find<T>(Control root, string name) where T : Control { return (T)root.Controls.Find(name, true).Single(); }
    static Dictionary<string, object> Obj(object value) { return (Dictionary<string, object>)value; }
    static void Check(bool value, string message) { ++checks; if (!value) throw new Exception(message); }
    static void Wait(Func<bool> done, string message) { var until = DateTime.UtcNow.AddSeconds(45); while (!done()) { if (DateTime.UtcNow > until) throw new Exception("Timeout: " + message); Application.DoEvents(); Thread.Sleep(10); } Application.DoEvents(); }
    static void Write(string file, object value) { File.WriteAllText(file, Json.Serialize(value), Utf8); }
    static string Hash(string file) { using (var hash = SHA256.Create()) using (var stream = File.OpenRead(file)) return BitConverter.ToString(hash.ComputeHash(stream)).Replace("-", "").ToLowerInvariant(); }
    static string Options(TraceWindow form) { return Json.Serialize(Invoke(form, "CaptureOptions")); }
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    static void Snapshot(Form form, string name) {
        Application.DoEvents(); form.Refresh();
        using (var bitmap = new Bitmap(form.Width, form.Height)) using (var graphics = Graphics.FromImage(bitmap)) {
            var dc = graphics.GetHdc(); try { Check(PrintWindow(form.Handle, dc, 2), "actual guided UI render"); } finally { graphics.ReleaseHdc(dc); }
            bitmap.Save(Path.Combine(evidence, name + ".png"), System.Drawing.Imaging.ImageFormat.Png);
        }
    }
    static void Dialog(TraceWindow form, string id, Action<Form> action) {
        Exception failed = null; bool observed = false;
        using (var timer = new System.Windows.Forms.Timer { Interval = 30 }) {
            timer.Tick += delegate {
                var dialog = Application.OpenForms.Cast<Form>().SingleOrDefault(item => item.Name == "GuidedCaptureDialog");
                if (dialog == null) return; timer.Stop(); observed = true;
                try { dialog.Opacity = 0; action(dialog); } catch (Exception error) { failed = error; dialog.DialogResult = DialogResult.Cancel; dialog.Close(); }
            };
            timer.Start(); Invoke(form, "BeginGuidedSetup", id);
        }
        Check(observed, "guide dialog was explicitly opened");
        if (failed != null) throw failed;
    }
    static void Fill(Form dialog, string output, string scope, string browser) {
        Find<TextBox>(dialog, "GuidedUrlInput").Text = "https://fixture.test/";
        Find<TextBox>(dialog, "GuidedChromeInput").Text = browser;
        Find<TextBox>(dialog, "GuidedOutputInput").Text = output;
        Find<TextBox>(dialog, "GuidedScopeInput").Text = scope;
    }
    static void Apply(TraceWindow form, string id, string output, string scope, string browser) {
        Dialog(form, id, dialog => { Fill(dialog, output, scope, browser); Check(Find<Button>(dialog, "GuidedApplyButton").Enabled, "explicit inputs permit preflight"); Find<Button>(dialog, "GuidedApplyButton").PerformClick(); });
        Wait(() => !(bool)Field("busy").GetValue(form) && Field("studioRequestId").GetValue(form) == null, "actual C++ preflight reply");
        Check(Field("captureDirectory").GetValue(form) == null && !Directory.Exists(output), "applying guide never starts capture or creates output");
    }
    [STAThread] public static int Main(string[] args) {
        evidence = args[0]; fixture = args[1]; var edition = args[2]; TraceWindow form = null; bool success = false; string error = "";
        try {
            var browser = Path.Combine(fixture, "browser"); Directory.CreateDirectory(browser);
            chrome = Path.Combine(browser, "chrome.exe"); File.WriteAllText(chrome, "fixture exe", Utf8);
            File.WriteAllText(Path.Combine(browser, "chrome.dll"), "fixture engine", Utf8);
            var descriptor = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(args[3], Utf8)); descriptor["edition"] = edition;
            descriptor["binaryHashes"] = new Dictionary<string, object> { { "chrome.exe", Hash(chrome) }, { "chrome.dll", Hash(Path.Combine(browser, "chrome.dll")) } };
            var descriptorFile = Path.Combine(browser, "v8trace-opcode.json"); Write(descriptorFile, descriptor);
            Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            form = new TraceWindow(Path.Combine(fixture, "theme.json")) { ShowInTaskbar = false, Opacity = 0 };
            form.Show(); Wait(() => (bool)Field("ready").GetValue(form), "actual edition runtime ready");
            Check(Find<Button>(form, "GuidedEnvironmentButton").Enabled && Find<Button>(form, "GuidedDetailButton").Enabled, "shared explicit guide actions ready");
            var original = Options(form);
            Dialog(form, "env-protocol", dialog => { Fill(dialog, Path.Combine(fixture, "cancelled"), "", chrome); Snapshot(dialog, "cancelled-baseline-dialog"); Find<Button>(dialog, "GuidedCancelButton").PerformClick(); });
            Check(Options(form) == original && !(bool)Field("busy").GetValue(form), "cancel leaves all current and manual options unchanged");
            Dialog(form, "env-target-detail", dialog => {
                Fill(dialog, Path.Combine(fixture, "invalid-scope"), "*\n??", chrome);
                Check(!Find<Button>(dialog, "GuidedApplyButton").Enabled && Find<Label>(dialog, "GuidedValidation").Text.Contains("具体"), "global-only scope is blocked before any mutation");
                dialog.Size = dialog.MinimumSize;
                foreach (var name in new[] { "GuidedUrlInput", "GuidedChromeInput", "GuidedOutputInput", "GuidedScopeInput" })
                    Check(!String.IsNullOrWhiteSpace(Find<TextBox>(dialog, name).AccessibleName), "named keyboard/accessibility input: " + name);
                Snapshot(dialog, "invalid-target-scope"); Find<Button>(dialog, "GuidedCancelButton").PerformClick();
            });
            Check(Options(form) == original, "invalid scope/cancel does not change settings");
            var output = Path.Combine(fixture, "baseline-output"); Apply(form, "env-protocol", output, "", chrome);
            var report = Json.Deserialize<Dictionary<string, object>>(Find<RichTextBox>(form, "SummaryText").Text);
            Check(Convert.ToBoolean(report["ok"]) && Convert.ToBoolean(report["browserVerified"]), "actual C++ descriptor/capability preflight, not browser startup");
            var options = Obj(Invoke(form, "CaptureOptions"));
            Check(Convert.ToString(options["preset"]) == "env-protocol" && Convert.ToString(options["chromePath"]) == chrome
                && Convert.ToBoolean(options["http"]) && Convert.ToBoolean(options["redirectBodies"]) && !Convert.ToBoolean(options["jsCallDetail"]), "baseline matches original preset and selected browser");
            Check(Find<Label>(form, "GuidedWorkflowStatus").Text.Contains("手动开始"), "visible explicit next action");
            output = Path.Combine(fixture, "detail-output"); Apply(form, "env-target-detail", output, "fixture.js", chrome);
            options = Obj(Invoke(form, "CaptureOptions"));
            Check(Convert.ToString(options["detailScriptInclude"]) == "fixture.js" && Convert.ToBoolean(options["jsCallValues"]) && Convert.ToBoolean(options["nativeDetail"]), "targeted scope retains full selected values");
            Apply(form, "env-protocol", Path.Combine(fixture, "missing-browser-output"), "", Path.Combine(fixture, "missing.exe"));
            report = Json.Deserialize<Dictionary<string, object>>(Find<RichTextBox>(form, "SummaryText").Text);
            Check(!Convert.ToBoolean(report["ok"]) && Convert.ToString(Obj(Invoke(form, "CaptureOptions"))["transport"]) == "native"
                && Find<Label>(form, "GuidedWorkflowStatus").Text.Contains("预检失败"), "missing browser is explicit; no CDP fallback or capture");
            descriptor.Remove("redirectBodyProtocol"); Write(descriptorFile, descriptor);
            Apply(form, "env-protocol", Path.Combine(fixture, "missing-capability-output"), "", chrome);
            report = Json.Deserialize<Dictionary<string, object>>(Find<RichTextBox>(form, "SummaryText").Text);
            Check(!Convert.ToBoolean(report["ok"]) && Convert.ToString(report["error"]).Contains("Redirect body protocol missing"), "unsupported capability remains rejected");
            var selector = Find<ComboBox>(form, "PresetInput"); selector.SelectedIndex = 0; Invoke(form, "ApplyPreset");
            var manual = Options(form);
            for (int n = 1; n < selector.Items.Count; ++n) {
                var record = Obj(selector.Items[n].GetType().GetField("Record").GetValue(selector.Items[n]));
                if (Convert.ToString(record["id"]) == "jsvmp-opcode") { selector.SelectedIndex = n; break; }
            }
            Check(Options(form) == manual, "selecting a different preset only previews and retains manual settings");
            Check(Find<Button>(form, "ApplyPresetButton").Enabled == (edition == "professional"), "unavailable advanced choice follows actual edition");
            if (edition == "community") Check(Find<RichTextBox>(form, "PresetHint").Text.Contains("Professional"), "visible compiled-edition restriction reason");
            var capture = Path.Combine(fixture, "capture"); Directory.CreateDirectory(Path.Combine(capture, "catalog")); Directory.CreateDirectory(Path.Combine(capture, "events"));
            var blobRoot = Path.Combine(capture, "blobs", "sha256"); Directory.CreateDirectory(blobRoot);
            var data = Encoding.UTF8.GetBytes("const fixture = '原值😀';\n"); var temporary = Path.Combine(fixture, "body"); File.WriteAllBytes(temporary, data); var hash = Hash(temporary); File.Copy(temporary, Path.Combine(blobRoot, hash));
            var reference = new { path = "blobs/sha256/" + hash, sha256 = hash, bytes = data.Length, representation = "fixture-source" };
            File.WriteAllText(Path.Combine(capture, "catalog", "resources.jsonl"), Json.Serialize(new { type = "Script", conclusion = "complete", requestId = "req-1",
                request = new { url = "https://fixture.test/fixture.js", method = "GET" }, response = new { status = 200, mimeType = "text/javascript" }, body = reference }) + "\n", Utf8);
            File.WriteAllText(Path.Combine(capture, "catalog", "scripts.jsonl"), Json.Serialize(new { scriptId = 1, url = "https://fixture.test/fixture.js", source = reference }) + "\n", Utf8);
            File.WriteAllText(Path.Combine(capture, "events", "env.jsonl"), "", Utf8);
            Write(Path.Combine(capture, "summary.json"), new { schemaVersion = 1, backend = "chromium-native", captureConclusion = "imported", integrityConclusion = "partial", criticalGapCount = 1 });
            Invoke(form, "BeginEvidenceCommand", "inspect", "guided fixture inspect", new Dictionary<string, object> { { "capture", capture } });
            Wait(() => !(bool)Field("busy").GetValue(form), "actual C++ inspect for guide next actions");
            Check(Find<Button>(form, "GuidedInspectButton").Enabled && Find<Button>(form, "GuidedExportButton").Enabled
                && Find<Label>(form, "GuidedWorkflowStatus").Text.Contains("关键缺口"), "partial capture offers inspection/export without claiming complete");
            Invoke(form, "ShowEvidenceSummary"); Check(Find<TabControl>(form, "EvidenceTabs").SelectedIndex == 1, "guide inspection uses existing summary/diagnostics");
            var exported = Path.Combine(fixture, "exported"); ((TextBox)Field("exportOutput").GetValue(form)).Text = exported;
            Invoke(form, "ExportCapture"); Wait(() => !(bool)Field("busy").GetValue(form), "explicit actual C++ export");
            Check(File.Exists(Path.Combine(exported, "capture.reference.json")) && Hash(Path.Combine(blobRoot, hash)) == hash, "explicit export creates referenced copy without changing raw source");
            Check(Find<Label>(form, "GuidedWorkflowStatus").Text.Contains("已导出"), "visible export next action");
            Snapshot(form, "guided-workflow-1280"); form.Size = new Size(1000, 720); Snapshot(form, "guided-workflow-1000");
            form.Close(); Wait(() => form.IsDisposed, "owned worker shutdown"); success = true;
        } catch (Exception failure) { error = failure.ToString(); Console.Error.WriteLine(error); }
        finally { if (form != null && !form.IsDisposed) { try { Invoke(form, "SetBusy", false); form.Close(); Wait(() => form.IsDisposed, "failure cleanup"); } catch { form.Dispose(); } } }
        Write(Path.Combine(evidence, "result.json"), new { protocol = "v8trace/ui-guided-workflow-test/1", edition = edition, status = success ? "passed" : "failed", checks = checks, error = error,
            uiSha256 = Hash(typeof(TraceWindow).Assembly.Location), actualCppWorker = true, browserLaunched = false, rawFixtureUnchanged = success, userPreferencesTouched = false, userLogsTouched = false });
        return success ? 0 : 1;
    }
}
