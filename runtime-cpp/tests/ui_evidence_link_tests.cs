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

public static class EvidenceLinkTests {
    static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 16777216 };
    static readonly UTF8Encoding Utf8 = new UTF8Encoding(false);
    static int checks;
    static string evidence, fixture;
    static FieldInfo Field(string name) { return typeof(TraceWindow).GetField(name, BindingFlags.NonPublic | BindingFlags.Instance); }
    static object Invoke(TraceWindow form, string name, params object[] values) { return typeof(TraceWindow).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Instance).Invoke(form, values); }
    static T Find<T>(Control root, string name) where T : Control { return (T)root.Controls.Find(name, true).Single(); }
    static Dictionary<string, object> Obj(object value) { return (Dictionary<string, object>)value; }
    static Dictionary<string, object> Clone(Dictionary<string, object> value) { return Json.Deserialize<Dictionary<string, object>>(Json.Serialize(value)); }
    static void Check(bool value, string message) { ++checks; if (!value) throw new Exception(message); }
    static void Wait(Func<bool> done, string message) { var until = DateTime.UtcNow.AddSeconds(60); while (!done()) { if (DateTime.UtcNow > until) throw new Exception("Timeout: " + message); Application.DoEvents(); Thread.Sleep(10); } Application.DoEvents(); }
    static void Idle(TraceWindow form) { Wait(() => !(bool)Field("busy").GetValue(form) && Field("studioRequestId").GetValue(form) == null, "actual C++ task and exact evidence read"); }
    static string Hash(string file) { using (var hash = SHA256.Create()) using (var stream = File.OpenRead(file)) return BitConverter.ToString(hash.ComputeHash(stream)).Replace("-", "").ToLowerInvariant(); }
    static string Options(TraceWindow form) { return Json.Serialize(Invoke(form, "CaptureOptions")); }
    static void Write(string file, object value) { File.WriteAllText(file, Json.Serialize(value), Utf8); }
    static void Workspace(TraceWindow form, string name) { var page = Find<TabPage>(form, name); ((TabControl)page.Parent).SelectedTab = page; }
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    static void Snapshot(Form form, string name) {
        Application.DoEvents(); form.Refresh();
        using (var bitmap = new Bitmap(form.Width, form.Height)) using (var graphics = Graphics.FromImage(bitmap)) {
            var dc = graphics.GetHdc(); try { Check(PrintWindow(form.Handle, dc, 2), "actual evidence-linked UI render"); } finally { graphics.ReleaseHdc(dc); }
            bitmap.Save(Path.Combine(evidence, name + ".png"), System.Drawing.Imaging.ImageFormat.Png);
        }
    }
    static void RecaptureDialog(TraceWindow form, Action<Form> action) {
        Exception failed = null; bool observed = false;
        using (var timer = new System.Windows.Forms.Timer { Interval = 30 }) {
            timer.Tick += delegate {
                var dialog = Application.OpenForms.Cast<Form>().SingleOrDefault(item => item.Name == "GuidedCaptureDialog");
                if (dialog == null) return; timer.Stop(); observed = true;
                try { dialog.Opacity = 0; action(dialog); } catch (Exception error) { failed = error; dialog.DialogResult = DialogResult.Cancel; dialog.Close(); }
            };
            timer.Start(); Invoke(form, "PrepareCausalRecapture");
        }
        Check(observed, "explicit recapture confirmation opened"); if (failed != null) throw failed;
    }
    [STAThread] public static int Main(string[] args) {
        evidence = args[0]; fixture = args[1]; var edition = args[2]; TraceWindow form = null; bool success = false; string error = "";
        try {
            var capture = Path.Combine(fixture, "causal-capture"); var before = Directory.GetFiles(capture, "*", SearchOption.AllDirectories).ToDictionary(p => p, p => Hash(p));
            Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            form = new TraceWindow(Path.Combine(fixture, "theme.json")) { ShowInTaskbar = false, Opacity = 0 };
            form.Show(); Wait(() => (bool)Field("ready").GetValue(form), "actual edition worker ready");
            Invoke(form, "BeginEvidenceCommand", "inspect", "owned evidence fixture", new Dictionary<string, object> { { "capture", capture } }); Idle(form);
            Check(Field("captureDirectory").GetValue(form) != null, "valid finalized capture accepted by actual inspect contract");
            Workspace(form, "CausalityTab");
            foreach (var name in new[] { "CausalBuild", "CausalOpenEvidence", "CausalOpenSource", "CausalRecapture", "CausalConfirmedVm" })
                Check(Find<Button>(form, name).Enabled == (edition == "professional"), "compiled edition gate: " + name);
            if (edition == "professional") {
                Find<Button>(form, "CausalBuild").PerformClick(); Idle(form);
                Check(Find<DataGridView>(form, "CausalRoots").Rows.Count == 1, "actual fixture graph root");
                Find<Button>(form, "CausalFunctions").PerformClick(); Idle(form);
                var functions = Find<DataGridView>(form, "CausalFunctionGrid"); Check(functions.Rows.Count == 3, "actual pre-request functions");
                functions.ClearSelection(); functions.Rows[1].Selected = true; Idle(form);
                var row = Clone(Obj(Field("causalObservedSelection").GetValue(form)));
                Check(Convert.ToString(row["requestPosition"]) == "observed-before-request" && Find<RichTextBox>(form, "CausalIdentity").Text.Contains("已观察发送位置"), "confirmed observation is not VM/algorithm confirmation");
                Check(Find<RichTextBox>(form, "CausalIdentity").Text.Contains("isolate-A") && Find<RichTextBox>(form, "CausalIdentity").Text.Contains(Convert.ToString(row["sourceFileSha256"])), "identity/hash context visible and exact");
                Snapshot(form, "professional-linked-flow"); form.Size = new Size(1000, 720); Snapshot(form, "professional-linked-flow-1000");
                Find<Button>(form, "CausalOpenEvidence").PerformClick(); Idle(form);
                var filters = (Dictionary<string, TextBox>)Field("indexFilters").GetValue(form);
                foreach (var pair in new[] { new[] { "request", "requestId" }, new[] { "process", "processId" }, new[] { "isolate", "isolateId" }, new[] { "window", "recordingWindow" }, new[] { "producer", "producerId" }, new[] { "call", "callId" } })
                    Check(filters[pair[0]].Text == Convert.ToString(row[pair[1]]), "observed identity propagated: " + pair[0]);
                var selected = Obj(Find<DataGridView>(form, "IndexGrid").SelectedRows[0].Tag);
                Check(Convert.ToString(selected["sourceFileSha256"]) == Convert.ToString(row["sourceFileSha256"]) && Convert.ToString(selected["line"]) == Convert.ToString(row["line"]), "independent viewer resolves identical original SHA and line, never reuses causal row generation");
                Check(Find<RichTextBox>(form, "IndexedDetails").Text.Contains("完整参数"), "exact raw value read after cross-view link");
                Invoke(form, "JumpIndexed", "request"); Idle(form);
                Check(filters["window"].Text == "2" && filters["isolate"].Text == "isolate-A", "request jump retains recording and isolate boundary");
                Workspace(form, "CausalityTab"); Find<Button>(form, "CausalOpenSource").PerformClick(); Idle(form);
                Check(Find<ComboBox>(form, "IndexCategoryInput").Text == "catalog/scripts" && filters["script"].Text == "7" && filters["isolate"].Text == "isolate-A" && filters["window"].Text == "", "source uses exact script/process/isolate; does not invent catalog window");
                Check(Find<DataGridView>(form, "IndexGrid").Rows.Count == 1 && Find<ComboBox>(form, "IndexedBlobReferences").Items.Count == 1, "source identity excludes other-isolate decoy and retains real reference");
                Find<Button>(form, "ReadBlobButton").PerformClick(); Idle(form);
                Check(Find<RichTextBox>(form, "IndexedDetails").Text.Contains("fixture-source-完整😀"), "exact source blob read through actual C++");
                Workspace(form, "CausalityTab"); var original = Options(form);
                RecaptureDialog(form, dialog => {
                    var description = Find<RichTextBox>(dialog, "GuidedPresetDescription").Text;
                    Check(description.Contains("开销：高") && description.Contains("预算") && description.Contains("Opcode=False") && description.Contains("sourceFileSha256"), "scope, cost, budgets and provenance shown before applying");
                    Check(Find<TextBox>(dialog, "GuidedScopeInput").Text == Convert.ToString(row["scriptUrl"]), "only actual concrete script is proposed");
                    Snapshot(dialog, "recapture-confirmation"); Find<Button>(dialog, "GuidedCancelButton").PerformClick();
                });
                Check(Options(form) == original, "cancel recapture retains all options and compiled channels");
                var browser = Path.Combine(fixture, "browser"); Directory.CreateDirectory(browser);
                var chrome = Path.Combine(browser, "chrome.exe"); File.WriteAllText(chrome, "fixture exe", Utf8); File.WriteAllText(Path.Combine(browser, "chrome.dll"), "fixture dll", Utf8);
                var descriptor = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(args[3], Utf8)); descriptor["edition"] = edition;
                descriptor["binaryHashes"] = new Dictionary<string, object> { { "chrome.exe", Hash(chrome) }, { "chrome.dll", Hash(Path.Combine(browser, "chrome.dll")) } }; Write(Path.Combine(browser, "v8trace-opcode.json"), descriptor);
                var output = Path.Combine(fixture, "deliberate-recapture");
                RecaptureDialog(form, dialog => {
                    Find<TextBox>(dialog, "GuidedUrlInput").Text = "https://fixture.test/"; Find<TextBox>(dialog, "GuidedChromeInput").Text = chrome; Find<TextBox>(dialog, "GuidedOutputInput").Text = output;
                    Find<Button>(dialog, "GuidedApplyButton").PerformClick();
                }); Idle(form);
                Check(Find<TabControl>(form, "WorkspaceTabs").SelectedIndex == 0, "confirmed recapture exposes actual preflight in capture workspace");
                var report = Json.Deserialize<Dictionary<string, object>>(Find<RichTextBox>(form, "SummaryText").Text);
                var options = Obj(Invoke(form, "CaptureOptions"));
                Check(Convert.ToBoolean(report["ok"]) && Convert.ToString(options["preset"]) == "sign-call" && !Convert.ToBoolean(options["opcode"]) && !Convert.ToBoolean(options["vmpProbe"]), "confirmed targeted setup only runs actual preflight, never enables full-site Opcode or VM probe");
                Check(!Directory.Exists(output), "preflight never creates the recapture directory");
                var loaded = Convert.ToString(Field("captureDirectory").GetValue(form));
                Check(String.Equals(Path.GetFullPath(loaded), Path.GetFullPath(capture), StringComparison.OrdinalIgnoreCase), "loaded capture identity preserved: " + loaded + " versus " + capture);
                original = Options(form); Invoke(form, "ShowConfirmedVmSettings"); Check(Options(form) == original, "opening confirmed-VM settings does not activate probe or manufacture descriptor");
                var missing = Clone(row); missing.Remove("isolateId"); Field("causalObservedSelection").SetValue(form, missing); Invoke(form, "LinkCausalEvidence", true);
                Check(!(bool)Field("busy").GetValue(form) && Find<Label>(form, "CausalStatus").Text.Contains("身份不足"), "missing isolate blocks source inference without broadening");
                missing = Clone(row); missing.Remove("scriptUrl"); Field("causalObservedSelection").SetValue(form, missing); Invoke(form, "PrepareCausalRecapture");
                Check(Options(form) == original && Find<Label>(form, "CausalStatus").Text.Contains("未观察到"), "absent script URL does not propose global recapture");
                var candidate = Clone(row); candidate["requestPosition"] = "causal-node-scope; request-position-unproven"; Invoke(form, "ShowCausalIdentity", candidate);
                Check(Find<RichTextBox>(form, "CausalIdentity").Text.Contains("候选关联"), "candidate is distinct from confirmed request position");
                candidate["sourceSha256Verified"] = true; candidate["bytecodeSha256Verified"] = true; candidate["sourceUtf16Sha256"] = new string('a', 64); candidate["bytecodeSha256"] = new string('b', 64); Invoke(form, "ShowCausalIdentity", candidate);
                Check(Find<RichTextBox>(form, "CausalIdentity").Text.Contains("已观察双哈希核验标志") && Options(form) == original, "VM verification flags remain observations, not automatic activation/semantics");
                var stale = Clone(row); stale["sourceFileSha256"] = new string('f', 64); Field("causalObservedSelection").SetValue(form, stale); Invoke(form, "LinkCausalEvidence", false); Idle(form);
                Check(Find<DataGridView>(form, "IndexGrid").SelectedRows.Count == 0 && Find<Label>(form, "IndexStatus").Text.Contains("证据不足"), "changed original SHA never selects a similar record");
            } else {
                var original = Options(form); Invoke(form, "LinkCausalEvidence", false); Invoke(form, "PrepareCausalRecapture"); Invoke(form, "ShowConfirmedVmSettings");
                Check(Options(form) == original && !(bool)Field("busy").GetValue(form) && Find<Label>(form, "CausalStatus").Text.Contains("Professional"), "direct shared-UI calls also preserve Community explanatory gate");
                Workspace(form, "IndexedEvidenceTab"); Invoke(form, "BuildIndex"); Idle(form);
                Check(Find<DataGridView>(form, "IndexGrid").Rows.Count > 0 && Field("viewerSelection").GetValue(form) != null, "Community raw evidence index remains fully usable");
                Snapshot(form, "community-raw-index");
            }
            Check(before.Count == Directory.GetFiles(capture, "*", SearchOption.AllDirectories).Length && before.All(pair => Hash(pair.Key) == pair.Value), "all original capture files byte-identical");
            form.Close(); Wait(() => form.IsDisposed, "owned worker EOF shutdown"); success = true;
        } catch (Exception failure) { error = failure.ToString(); Console.Error.WriteLine(error); }
        finally { if (form != null && !form.IsDisposed) { try { Invoke(form, "SetBusy", false); form.Close(); Wait(() => form.IsDisposed, "failure cleanup"); } catch { form.Dispose(); } } }
        Write(Path.Combine(evidence, "result.json"), new { protocol = "v8trace/ui-evidence-links-test/1", edition = edition, status = success ? "passed" : "failed", checks = checks, error = error,
            uiSha256 = Hash(typeof(TraceWindow).Assembly.Location), actualCppWorker = true, browserLaunched = false, rawCaptureUnchanged = success, userPreferencesTouched = false });
        return success ? 0 : 1;
    }
}
