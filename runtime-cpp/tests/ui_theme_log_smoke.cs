using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using V8Trace;

public static class ThemeLogSmoke {
    static int checks;
    static string evidence, fixture, edition;
    static void Check(bool condition, string message) { ++checks; if (!condition) throw new Exception(message); }
    static T Find<T>(Control parent, string name) where T : Control { var controls = parent.Controls.Find(name, true); Check(controls.Length == 1, "Unique control " + name); return (T)controls[0]; }
    static FieldInfo Field(string name) { return typeof(TraceWindow).GetField(name, BindingFlags.NonPublic | BindingFlags.Instance); }
    static object Invoke(Form form, string name, params object[] values) { return typeof(TraceWindow).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Instance).Invoke(form, values); }
    static void Wait(Func<bool> condition, string description) { var deadline = DateTime.UtcNow.AddSeconds(45); while (!condition()) { if (DateTime.UtcNow > deadline) throw new Exception("Timeout: " + description); Application.DoEvents(); Thread.Sleep(15); } Application.DoEvents(); }
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] static extern IntPtr GetFocus();
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    static Bitmap ControlImage(Control control, string name) {
        control.Refresh(); Application.DoEvents(); var bitmap = new Bitmap(control.Width, control.Height);
        using(var graphics=Graphics.FromImage(bitmap)) { var dc=graphics.GetHdc(); try { Check(PrintWindow(control.Handle,dc,2),"Actual control screenshot "+name); } finally { graphics.ReleaseHdc(dc); } }
        bitmap.Save(Path.Combine(evidence,name+".png"),System.Drawing.Imaging.ImageFormat.Png);return bitmap;
    }
    static int ChangedPixels(Bitmap left, Bitmap right, int height) {
        Check(left.Size==right.Size,"Compared frames have identical bounds");int count=0;
        for(int y=0;y<Math.Min(height,left.Height);++y)for(int x=0;x<left.Width;++x) {
            var a=left.GetPixel(x,y);var b=right.GetPixel(x,y);
            if(Math.Abs(a.R-b.R)+Math.Abs(a.G-b.G)+Math.Abs(a.B-b.B)>18)count++;
        }
        return count;
    }
    static void NativeClick(Form form, CheckBox check, int x) {
        form.BringToFront();form.Activate();check.Focus();Application.DoEvents();
        var point=new Point(x,check.Height/2);Check(WindowFromPoint(check.PointToScreen(point))==check.Handle,"Mouse click targets the owned checkbox, not another window");
        var position=new IntPtr((point.Y<<16)|(point.X&0xffff));
        SendMessage(check.Handle,0x0201,new IntPtr(1),position);SendMessage(check.Handle,0x0202,IntPtr.Zero,position);Application.DoEvents();
    }
    static void AppearanceBounds(Form form, string name) {
        var panel=Find<TableLayoutPanel>(form,"ThemeSettings");var bounds=panel.RectangleToScreen(panel.ClientRectangle);
        Check(!Find<FlowLayoutPanel>(form,"WorkspaceNavigation").HorizontalScroll.Visible,"Sidebar navigation has no accidental horizontal scrollbar "+name);
        foreach(var id in new[]{"ThemeSelector","ThemeAccentButton","ThemeSecondaryButton","ThemeResetButton","ThemeMotionToggle","ThemeStatus"}) {
            var control=Find<Control>(form,id);var actual=control.RectangleToScreen(control.ClientRectangle);Check(control.Visible&&bounds.Contains(actual),"Appearance control contained "+id+" / "+name);
            for(Control parent=control.Parent;parent!=null;parent=parent.Parent)Check(parent.RectangleToScreen(parent.ClientRectangle).Contains(actual),"No ancestor clips appearance control "+id+" / "+name+" / "+parent.Name);
        }
        var primary=Find<Button>(form,"ThemeAccentButton");var secondary=Find<Button>(form,"ThemeSecondaryButton");
        Check(Math.Abs(primary.Width-secondary.Width)<=1&&primary.Height==secondary.Height,"Equal color action sizes "+name);
        Check(primary.RectangleToScreen(primary.ClientRectangle).Right<secondary.RectangleToScreen(secondary.ClientRectangle).Left,"Color actions have a visible gutter "+name);
        var selector=Find<ComboBox>(form,"ThemeSelector");var reset=Find<Button>(form,"ThemeResetButton");var motion=Find<CheckBox>(form,"ThemeMotionToggle");
        Check(selector.RectangleToScreen(selector.ClientRectangle).Left==reset.RectangleToScreen(reset.ClientRectangle).Left&&reset.RectangleToScreen(reset.ClientRectangle).Left==motion.RectangleToScreen(motion.ClientRectangle).Left,"Appearance rows aligned "+name);
        Check(selector.Width==reset.Width&&reset.Width==motion.Width&&selector.RectangleToScreen(selector.ClientRectangle).Right==secondary.RectangleToScreen(secondary.ClientRectangle).Right,"Appearance rows share the same right edge "+name);
        Check(Math.Abs(primary.Height-reset.Height)<=1&&Math.Abs(selector.Height-reset.Height)<=2,"Theme and palette action heights consistent "+name);
        Check(motion.Height>=TextRenderer.MeasureText(motion.Text,motion.Font).Height+8,"Large motion input target "+name);
        using(var image=ControlImage(panel,"appearance-"+name)){}
    }
    static void Snapshot(Form form, string name) {
        Application.DoEvents(); form.Refresh();
        using (var bitmap = new Bitmap(form.Width, form.Height)) using (var graphics = Graphics.FromImage(bitmap)) { var dc = graphics.GetHdc(); try { Check(PrintWindow(form.Handle, dc, 2), "Actual screenshot " + name); } finally { graphics.ReleaseHdc(dc); } bitmap.Save(Path.Combine(evidence, name + ".png"), System.Drawing.Imaging.ImageFormat.Png); }
    }
    static void Confirm(Form form, bool permanent, bool accept) {
        Exception failure = null;
        using (var closer = new System.Windows.Forms.Timer { Interval = 60 }) {
            closer.Tick += delegate {
                var dialog = Application.OpenForms.Cast<Form>().FirstOrDefault(item => item.Name == "LogCleanupConfirmation"); if (dialog == null) return;
                closer.Stop();
                try {
                    var confirm = Find<Button>(dialog, "LogConfirmCleanup");
                    Check(confirm.Enabled == !permanent, "Permanent mode requires keyword; recycle mode does not");
                    if (permanent) { var keyword = Find<TextBox>(dialog, "LogPermanentConfirmation"); keyword.Text = "delete"; Check(!confirm.Enabled, "Exact DELETE keyword"); keyword.Text = "DELETE"; Check(confirm.Enabled, "Permanent confirmation unlocked explicitly"); }
                    Snapshot(dialog, permanent ? "log-permanent-confirmation" : "log-recycle-confirmation");
                    if (accept) confirm.PerformClick(); else ((Button)dialog.CancelButton).PerformClick();
                } catch (Exception error) { failure = error; dialog.DialogResult = DialogResult.Cancel; dialog.Close(); }
            };
            closer.Start(); Find<Button>(form, "LogClearSelectedButton").PerformClick();
        }
        if (failure != null) throw failure;
    }
    [STAThread] public static int Main(string[] args) {
        evidence = args[0]; fixture = args[1]; edition = args[2]; var settings = Path.Combine(fixture, "theme.json");
        bool success = false; string error = ""; TraceWindow form = null;
        try {
            Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            form = new TraceWindow(settings); form.Show(); Wait(() => (bool)Field("ready").GetValue(form), "Real C++ worker ready");
            Check(form.Text.Contains(edition == "professional" ? "Professional" : "Community"), "Actual compiled edition identity");
            Check(form.Icon != null && form.Controls.Find("AlcoholBrandMark", true).Length == 0, "Keep EXE icon; no large header picture");
            Check(Find<Label>(form, "EditionBadge").Text.Contains(edition.ToUpperInvariant()), "Edition badge follows backend, not theme");
            var selector = Find<ComboBox>(form, "ThemeSelector"); Check(selector.Items.Count == 8 && ((ThemeDefinition)selector.SelectedItem).Id == "phantom", "Eight styles, existing phantom default retained");
            var section = Find<ComboBox>(form, "CaptureSectionSelector"); Check(section.SelectedIndex == 0 && !String.IsNullOrEmpty(section.Text), "Visible capture-section selection");
            var configuration = Find<TabControl>(form, "ConfigTabs"); section.SelectedIndex = 2; Check(configuration.SelectedIndex == 2, "Capture section navigation"); section.SelectedIndex = 0;
            var motion = Find<CheckBox>(form, "ThemeMotionToggle");
            for (int index = 0; index < selector.Items.Count; ++index) {
                selector.SelectedIndex = index; Application.DoEvents(); var theme = (ThemeDefinition)selector.SelectedItem;
                Check(CyberPalette.Theme.Id == theme.Id && form.BackColor.ToArgb() == theme.Background.ToArgb(), "Applied theme " + theme.Id);
                Check(Find<Label>(form, "BrandTitle").Text == "V8 TRACE / 浏览器采集" && Find<Label>(form, "ThemeCaption").Text == "ALCOHOL   /   V8 TRACE / 浏览器采集", "Unified title across every theme " + theme.Id);
                if (index > 0) Check(ThemePreferences.Load(settings).Selected == theme.Id, "Theme switch persisted " + theme.Id);
                Check(motion.Enabled && motion.AutoCheck && motion is ThemeMotionCheckBox, "Observable motion preference remains clickable in every theme " + theme.Id);
                var animation = (System.Windows.Forms.Timer)Field("themeAnimation").GetValue(form);
                Wait(() => animation.Enabled == (theme.Id.StartsWith("phantom", StringComparison.Ordinal) && CyberPalette.SystemAllowsMotion), "System animation notification settled " + theme.Id);
                var systemMotion = CyberPalette.SystemAllowsMotion;
                Check(animation.Enabled == (theme.Id.StartsWith("phantom", StringComparison.Ordinal) && systemMotion), "Motion obeys OS preferences " + theme.Id + "; timer=" + animation.Enabled + "; system=" + systemMotion + "; visible=" + form.Visible + "; window=" + form.WindowState + "; preference=" + ((ThemePreferences)Field("themePreferences").GetValue(form)).Motion);
                var header = Find<CyberHeaderPanel>(form, "CyberHeader"); var caption = Find<Label>(form, "ThemeCaption");
                Check(header.RectangleToScreen(header.ClientRectangle).Contains(caption.RectangleToScreen(caption.ClientRectangle)), "Header caption not clipped " + theme.Id);
                form.Size = new Size(1280, 860); Snapshot(form, theme.Id + "-desktop");AppearanceBounds(form,theme.Id+"-desktop");
                form.Size = new Size(1000, 720); Snapshot(form, theme.Id + "-narrow");AppearanceBounds(form,theme.Id+"-narrow");
                var byteLabel = (Label)Field("byteCount").GetValue(form); Check(TextRenderer.MeasureText(byteLabel.Text, byteLabel.Font).Width <= byteLabel.ClientSize.Width, "Compact metrics fit " + theme.Id);
                Check(Find<Button>(form, "ThemeResetButton").ClientSize.Height >= 20 && Find<CheckBox>(form, "ThemeMotionToggle").Visible, "Theme actions reachable after resize " + theme.Id);
                Check(Find<Label>(form, "EditionBadge").Text.Contains(edition.ToUpperInvariant()), "Theme keeps edition identity " + theme.Id);
                form.Size = new Size(1280, 860);
                NativeClick(form,motion,8);Check(!motion.Checked&&!ThemePreferences.Load(settings).Motion&&!animation.Enabled&&((ThemeMotionCheckBox)motion).StateText=="已关闭","Actual glyph click turns off and persists "+theme.Id);
                using(var off=ControlImage(motion,theme.Id+"-checkbox-off")) {
                    NativeClick(form,motion,Math.Min(motion.Width-8,motion.Font.Height+36));
                    Check(motion.Checked&&ThemePreferences.Load(settings).Motion,"Actual text click turns on and persists "+theme.Id);
                    using(var on=ControlImage(motion,theme.Id+"-checkbox-on"))Check(ChangedPixels(off,on,on.Height)>40,"Checked/unchecked glyph and state visibly differ "+theme.Id);
                }
                if(theme.Id.StartsWith("phantom",StringComparison.Ordinal)&&systemMotion) {
                    using(var first=ControlImage(header,theme.Id+"-effect-a")) {
                        var beforePhase=header.AnimationPhase;Wait(()=>header.AnimationPhase-beforePhase>0.08,"Visible effect advances "+theme.Id);
                        using(var second=ControlImage(header,theme.Id+"-effect-b")) {
                            Check(ChangedPixels(first,second,first.Height)>first.Width/2,"Stronger effect changes a visible area "+theme.Id);
                            if(theme.Id=="phantom-smoke")Check(ChangedPixels(first,second,Math.Max(0,first.Height-24))>first.Width,"Smoke changes above the narrow neon band");
                        }
                    }
                }
            }
            motion.Focus();form.Activate();Application.DoEvents();Check(GetFocus()==motion.Handle&&GetForegroundWindow()==form.Handle,"Keyboard targets only the owned test checkbox");
            bool keyboardBefore=motion.Checked;SendKeys.SendWait(" ");Application.DoEvents();Check(motion.Checked!=keyboardBefore&&ThemePreferences.Load(settings).Motion==motion.Checked,"Actual Space key toggles and persists");
            motion.AccessibilityObject.DoDefaultAction();Application.DoEvents();Check(motion.Checked==keyboardBefore&&ThemePreferences.Load(settings).Motion==motion.Checked,"Accessible default action toggles and persists");
            selector.SelectedIndex = 0; motion.Checked = false; Check(!((System.Windows.Forms.Timer)Field("themeAnimation").GetValue(form)).Enabled && !ThemePreferences.Load(settings).Motion, "Manual animation-off saves and stops repaint");
            using(var first=ControlImage(Find<CyberHeaderPanel>(form,"CyberHeader"),"motion-off-a")) {
                Thread.Sleep(250);Application.DoEvents();using(var second=ControlImage(Find<CyberHeaderPanel>(form,"CyberHeader"),"motion-off-b"))Check(ChangedPixels(first,second,first.Height)==0,"Off stops all decorative frame changes");
            }
            motion.Checked = true;
            if (CyberPalette.SystemAllowsMotion) {
                var header = Find<CyberHeaderPanel>(form, "CyberHeader"); var before = header.AnimationPhase;
                Wait(() => header.AnimationPhase != before, "Real animation advances");
            }
            form.WindowState = FormWindowState.Minimized; Application.DoEvents(); Check(!((System.Windows.Forms.Timer)Field("themeAnimation").GetValue(form)).Enabled, "Minimized stops decoration timer"); form.WindowState = FormWindowState.Normal;
            form.Hide(); Application.DoEvents(); Check(!((System.Windows.Forms.Timer)Field("themeAnimation").GetValue(form)).Enabled, "Hidden window stops decoration timer"); form.Show(); Application.DoEvents(); Check(((System.Windows.Forms.Timer)Field("themeAnimation").GetValue(form)).Enabled == CyberPalette.SystemAllowsMotion, "Shown again resumes only when system permits");
            var preferences = (ThemePreferences)Field("themePreferences").GetValue(form); preferences.SetColors(Color.Black, Color.White); Invoke(form, "ApplyCyberTheme", true);
            Check(Find<Button>(form, "ThemeAccentButton").BackColor.ToArgb() == Color.Black.ToArgb() && CyberPalette.Contrast(CyberPalette.Cyan, CyberPalette.Surface) >= 4.5, "Custom color applied with readable labels");
            Find<Button>(form, "ThemeResetButton").PerformClick(); Check(ThemePreferences.Load(settings).Primary.ToArgb() == ThemePreferences.Themes[0].Primary.ToArgb(), "UI reset palette action");

            var logs = Path.Combine(fixture, "logs"); var active = Path.Combine(logs, "current"); var available = Path.Combine(logs, "history");
            Directory.CreateDirectory(active); Directory.CreateDirectory(available); File.WriteAllText(Path.Combine(active, "trace.jsonl"), "current"); File.WriteAllText(Path.Combine(available, "trace.jsonl"), "history"); File.WriteAllText(Path.Combine(logs, "single.jsonl"), "single");
            bool recycleFlag = false;
            Field("logStorage").SetValue(form, new LogStorage(Path.GetDirectoryName(typeof(TraceWindow).Assembly.Location), Path.Combine(fixture, "audit.jsonl"), delegate(string path, bool recycle) { recycleFlag = recycle; if (Directory.Exists(path)) Directory.Move(path, Path.Combine(fixture, "recycled-history")); else File.Move(path, Path.Combine(fixture, "recycled-single")); }));
            Field("captureDirectory").SetValue(form, active);
            Find<Button>(form, "Navigate_LogManagementTab").PerformClick(); Check(Find<TabControl>(form, "WorkspaceTabs").SelectedTab.Name == "LogManagementTab", "Sidebar opens full-width log manager");
            var logRoot = Find<TextBox>(form, "LogRootInput"); logRoot.Text = logs; Find<Button>(form, "LogRefreshButton").PerformClick();
            Wait(() => !(bool)Field("logsScanning").GetValue(form) && Field("logSnapshot").GetValue(form) != null, "Background log scan");
            var grid = Find<DataGridView>(form, "LogTable"); Check(grid.Rows.Count == 3, "Exact fixture list");
            foreach (var name in new[] { "LogBrowseButton", "LogOpenDirectoryButton", "LogRefreshButton" }) { var action = Find<Button>(form, name); Check(action.ClientSize.Height >= TextRenderer.MeasureText(action.Text, action.Font).Height + 4, "Log path action text not clipped " + name); }
            var current = grid.Rows.Cast<DataGridViewRow>().Single(row => ((LogStorageItem)row.Tag).Name == "current"); Check(current.Cells[0].ReadOnly && !((LogStorageItem)current.Tag).CanClean, "Loaded capture protected");
            Check(Find<CheckBox>(form, "LogUseRecycleBin").Checked, "Recycle is default");
            Find<CheckBox>(form, "LogSelectAll").Checked = true; Check(Find<Button>(form, "LogClearSelectedButton").Text.Contains("(2)"), "Select all only eligible rows");
            Invoke(form, "SetBusy", true); Check(!Find<Button>(form, "LogClearAllButton").Enabled, "Cleanup held while capture/worker is busy"); Invoke(form, "SetBusy", false);
            Snapshot(form, "log-management-populated");
            Find<CheckBox>(form, "LogSelectAll").Checked = false; var rowToRemove = grid.Rows.Cast<DataGridViewRow>().Single(row => ((LogStorageItem)row.Tag).Name == "history"); rowToRemove.Cells[0].Value = true;
            Confirm(form, false, true);
            Wait(() => !(bool)Field("logsDeleting").GetValue(form) && !(bool)Field("logsScanning").GetValue(form) && grid.Rows.Count == 2, "Confirmed background cleanup and refresh");
            Check(recycleFlag && File.Exists(Path.Combine(fixture, "recycled-history", "trace.jsonl")) && File.Exists(Path.Combine(active, "trace.jsonl")), "Only chosen fixture recycled; active evidence retained");
            Check(Find<Label>(form, "LogStatus").Text.Contains("完成 1"), "Cleanup result survives asynchronous refresh");
            grid.Rows.Cast<DataGridViewRow>().Single(row => ((LogStorageItem)row.Tag).Name == "single.jsonl").Cells[0].Value = true; Find<CheckBox>(form, "LogUseRecycleBin").Checked = false;
            Confirm(form, true, false); Check(File.Exists(Path.Combine(logs, "single.jsonl")), "Cancelled permanent UI confirmation preserves file");
            logRoot.Text = Path.Combine(fixture, "missing"); Check(!Find<Button>(form, "LogClearSelectedButton").Enabled && grid.Rows.Count == 0, "Changed root invalidates old selections");
            Find<Button>(form, "LogRefreshButton").PerformClick(); Wait(() => !(bool)Field("logsScanning").GetValue(form), "Missing log root feedback"); Check(Find<Label>(form, "LogEmptyState").Visible && !Find<Button>(form, "LogClearAllButton").Enabled, "Missing root is visible and non-destructive"); Snapshot(form, "log-directory-missing");
            form.Close(); Wait(() => form.IsDisposed, "Own C++ worker clean shutdown");
            success = true;
        } catch (Exception failure) { error = failure.ToString(); Console.Error.WriteLine(error); }
        finally { if (form != null && !form.IsDisposed) { try { Invoke(form, "SetBusy", false); form.Close(); Wait(() => form.IsDisposed, "Failure cleanup shutdown"); } catch (Exception) { form.Dispose(); } } }
        File.WriteAllText(Path.Combine(evidence, "result.json"), new JavaScriptSerializer().Serialize(new { protocol = "v8trace/ui-theme-log-smoke/1", edition = edition, status = success ? "passed" : "failed", checks = checks, error = error, themes = ThemePreferences.Themes.Length, themeIds = ThemePreferences.Themes.Select(theme=>theme.Id).ToArray(), nativeMouseInputExercised = success, keyboardAndAccessibilityExercised = success, browserLaunched = false, userPreferencesTouched = false, realUserLogsTouched = false, uiSha256 = Hash(typeof(TraceWindow).Assembly.Location) }), new UTF8Encoding(false));
        return success ? 0 : 1;
    }
    static string Hash(string file) { using (var hash = System.Security.Cryptography.SHA256.Create()) using (var stream = File.OpenRead(file)) return BitConverter.ToString(hash.ComputeHash(stream)).Replace("-", "").ToLowerInvariant(); }
}
