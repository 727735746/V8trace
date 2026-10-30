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

public static class ShareCopyTests {
    static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 16777216 };
    static readonly UTF8Encoding Utf8 = new UTF8Encoding(false);
    static int checks;
    static string evidence;
    static FieldInfo Field(string name) { return typeof(TraceWindow).GetField(name, BindingFlags.NonPublic | BindingFlags.Instance); }
    static object Invoke(TraceWindow form, string name, params object[] values) { return typeof(TraceWindow).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Instance).Invoke(form, values); }
    static T Find<T>(Control root, string name) where T : Control { return (T)root.Controls.Find(name, true).Single(); }
    static void Check(bool value, string message) { ++checks; if (!value) throw new Exception(message); }
    static void Wait(Func<bool> done, string message) { var until = DateTime.UtcNow.AddSeconds(60); while (!done()) { if (DateTime.UtcNow > until) throw new Exception("Timeout: " + message); Application.DoEvents(); Thread.Sleep(10); } Application.DoEvents(); }
    static void Idle(TraceWindow form) { Wait(() => !(bool)Field("busy").GetValue(form) && Field("studioRequestId").GetValue(form) == null, "actual sharing-copy worker reply"); }
    static string Hash(string file) { using (var hash = SHA256.Create()) using (var stream = File.OpenRead(file)) return BitConverter.ToString(hash.ComputeHash(stream)).Replace("-", "").ToLowerInvariant(); }
    static void Write(string file, object value) { File.WriteAllText(file, Json.Serialize(value), Utf8); }
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    static void Snapshot(Form form, string name) {
        Application.DoEvents(); form.Refresh();
        using (var bitmap = new Bitmap(form.Width, form.Height)) using (var graphics = Graphics.FromImage(bitmap)) {
            var dc = graphics.GetHdc(); try { Check(PrintWindow(form.Handle, dc, 2), "actual sharing-copy UI render"); } finally { graphics.ReleaseHdc(dc); }
            bitmap.Save(Path.Combine(evidence, name + ".png"), System.Drawing.Imaging.ImageFormat.Png);
        }
    }
    static void Dialog(TraceWindow form, Action<Form> action) {
        Exception failed = null; bool observed = false;
        using (var timer = new System.Windows.Forms.Timer { Interval = 30 }) {
            timer.Tick += delegate {
                var dialog = Application.OpenForms.Cast<Form>().SingleOrDefault(item => item.Name == "SharingCopyDialog");
                if (dialog == null) return; timer.Stop(); observed = true;
                try { dialog.Opacity = 0; action(dialog); } catch (Exception error) { failed = error; Invoke(form, "Send", "stop", null); Idle(form); dialog.Close(); }
            };
            timer.Start(); Invoke(form, "ShowSharingCopy");
        }
        Check(observed, "sharing-copy dialog explicitly opened"); if (failed != null) throw failed;
    }
    [STAThread] public static int Main(string[] args) {
        evidence = args[0]; var fixture = args[1]; var edition = args[2]; TraceWindow form = null; bool success = false; string error = "";
        try {
            var capture = Path.Combine(fixture, "capture"); Directory.CreateDirectory(capture);
            Write(Path.Combine(capture, "summary.json"), new { schemaVersion = 1, backend = "chromium-native", captureConclusion = "imported", integrityConclusion = "partial", criticalGapCount = 1 });
            var original = Path.Combine(fixture, "original-export"); Directory.CreateDirectory(original);
            Write(Path.Combine(original, "export.summary.json"), new { schemaVersion = 1, conclusion = "exported-pending-entry", captureIntegrity = "partial" });
            Write(Path.Combine(original, "payload.json"), new { token = "raw-secret-one", items = new[] { new { Token = "raw-secret-two", password = "raw-password", value = "完整值😀" } } });
            File.WriteAllBytes(Path.Combine(original, "opaque.bin"), Enumerable.Range(0, 65536).Select(n => (byte)n).ToArray());
            var before = Directory.GetFiles(original).Concat(Directory.GetFiles(capture)).ToDictionary(p => p, p => Hash(p));
            Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            form = new TraceWindow(Path.Combine(fixture, "theme.json")) { ShowInTaskbar = false, Opacity = 0 }; form.Show();
            Wait(() => (bool)Field("ready").GetValue(form), "actual compiled edition ready");
            Check(!Find<Button>(form, "SharingCopyButton").Enabled, "no sharing action without loaded evidence");
            Invoke(form, "BeginEvidenceCommand", "inspect", "owned capture", new Dictionary<string, object> { { "capture", capture } }); Idle(form);
            Check(Find<Button>(form, "SharingCopyButton").Enabled, "same explicit sharing action available in both editions");
            var output = Path.Combine(fixture, "sharing-output"); var options = Json.Serialize(Invoke(form, "CaptureOptions"));
            Dialog(form, dialog => {
                Find<TextBox>(dialog, "ShareSourceInput").Text = original; Find<TextBox>(dialog, "ShareOutputInput").Text = output;
                var rules = Find<RichTextBox>(dialog, "ShareFieldSettings"); rules.Text = "{"; Find<Button>(dialog, "SharePreviewButton").PerformClick();
                Check(!Find<Button>(dialog, "ShareCreateButton").Enabled && !Directory.Exists(output), "malformed field settings never start copying");
                rules.Text = Json.Serialize(new { protocol = "v8trace/share-fields/1", copyOpaque = false, rules = new object[] { new { field = "token", action = "replace", value = "[copy]" }, new { field = "password", action = "remove" } } });
                Find<Button>(dialog, "SharePreviewButton").PerformClick(); Check(!Find<Button>(dialog, "ShareCreateButton").Enabled, "creation gated until actual preview completes"); Idle(form);
                var preview = Find<RichTextBox>(dialog, "ShareFieldPreview").Text;
                Check(preview.Contains("affectedFieldCounts") && preview.Contains("opaque-content-not-selected") && !preview.Contains("raw-secret"), "preview shows actual fields/omissions, not original values");
                Check(!Directory.Exists(output) && Find<Button>(dialog, "ShareCreateButton").Enabled, "preview is read-only and requires explicit creation");
                rules.Text += " "; Check(!Find<Button>(dialog, "ShareCreateButton").Enabled, "editing settings invalidates reviewed copy action");
                Find<Button>(dialog, "SharePreviewButton").PerformClick(); Idle(form); Snapshot(dialog, "sharing-field-preview");
                dialog.Size = dialog.MinimumSize; Snapshot(dialog, "sharing-field-preview-minimum");
                Find<Button>(dialog, "ShareCreateButton").PerformClick(); Idle(form);
                var manifest = Path.Combine(output, "share.manifest.json"); Check(File.Exists(manifest), "actual C++ creates independent manifest");
                var manifestHash = Hash(manifest); var processed = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(Path.Combine(output, "content", "payload.json"), Utf8));
                Check(Convert.ToString(processed["token"]) == "[copy]" && !File.Exists(Path.Combine(output, "content", "opaque.bin")), "selected fields transformed and unselected opaque body omitted");
                Check(Find<Label>(dialog, "ShareCopyStatus").Text.Contains("分享副本已创建"), "visible derived-only completion");
                Find<Button>(dialog, "SharePreviewButton").PerformClick(); Idle(form); Find<Button>(dialog, "ShareCreateButton").PerformClick(); Idle(form);
                Check(Find<Label>(dialog, "ShareCopyStatus").Text.Contains("任务未完成") && Hash(manifest) == manifestHash, "existing destination rejection retains prior independent copy exactly");
                Find<Button>(dialog, "ShareCloseButton").PerformClick();
            });
            Check(Json.Serialize(Invoke(form, "CaptureOptions")) == options, "sharing UI never mutates capture configuration");
            Dialog(form, dialog => { Find<Button>(dialog, "SharePreviewButton").PerformClick(); dialog.Close(); });
            Idle(form); Check(before.All(pair => Hash(pair.Key) == pair.Value), "capture, original export and all original hashes unchanged");
            form.Close(); Wait(() => form.IsDisposed, "owned worker EOF shutdown"); success = true;
        } catch (Exception failure) { error = failure.ToString(); Console.Error.WriteLine(error); }
        finally { if (form != null && !form.IsDisposed) { try { Invoke(form, "SetBusy", false); form.Close(); Wait(() => form.IsDisposed, "failure cleanup"); } catch { form.Dispose(); } } }
        Write(Path.Combine(evidence, "result.json"), new { protocol = "v8trace/ui-share-copy-test/1", edition = edition, status = success ? "passed" : "failed", checks = checks, error = error,
            uiSha256 = Hash(typeof(TraceWindow).Assembly.Location), runtimeSha256 = Hash(Path.Combine(Path.GetDirectoryName(typeof(TraceWindow).Assembly.Location), "runtime", "v8trace-runtime.exe")), actualCppWorker = true, browserLaunched = false, rawCaptureAndExportUnchanged = success, userPreferencesTouched = false });
        return success ? 0 : 1;
    }
}
