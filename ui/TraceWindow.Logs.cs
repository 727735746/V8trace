using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace V8Trace {
    public sealed partial class TraceWindow {
        readonly TextBox logRoot = new TextBox { Name = "LogRootInput", Dock = DockStyle.Fill };
        readonly Label logSummary = new Label { Name = "LogSummary", Dock = DockStyle.Fill, AutoEllipsis = true };
        readonly Label logStatus = new Label { Name = "LogStatus", Dock = DockStyle.Fill, AutoEllipsis = true, Text = "选择日志目录，按需浏览与清理。" };
        readonly DataGridView logGrid = new DataGridView { Name = "LogTable", Dock = DockStyle.Fill, AllowUserToAddRows = false, AllowUserToDeleteRows = false,
            RowHeadersVisible = false, SelectionMode = DataGridViewSelectionMode.FullRowSelect, MultiSelect = true, AutoSizeRowsMode = DataGridViewAutoSizeRowsMode.None };
        readonly Label logEmpty = new Label { Name = "LogEmptyState", Dock = DockStyle.Fill, Text = "LOG ARCHIVE / 等待日志目录\n\n选择一个采集目录，查看体积、时间与占用状态。", TextAlign = ContentAlignment.MiddleCenter };
        readonly CheckBox recycleLogs = new CheckBox { Name = "LogUseRecycleBin", Checked = true, Text = "移至回收站", AutoSize = true };
        readonly Button logClearAll = new Button { Name = "LogClearAllButton", Text = "全部清理", AutoSize = true };
        readonly Button logClearSelected = new Button { Name = "LogClearSelectedButton", Text = "清理选中 (0)", AutoSize = true };
        readonly Button logClearLarge = new Button { Name = "LogClearLargeButton", Text = "清理 >1 GB", AutoSize = true };
        readonly Button logCancel = new Button { Name = "LogCancelButton", Text = "取消", AutoSize = true, Visible = false };
        readonly List<Control> logPathControls = new List<Control>();
        readonly CheckBox logSelectAll = new CheckBox { Name = "LogSelectAll", Text = "全选可清理项", AutoSize = true };
        LogStorage logStorage;
        LogStorageSnapshot logSnapshot;
        CancellationTokenSource logScanCancellation, logDeleteCancellation;
        bool logsScanning, logsDeleting, logRowsChanging;
        int logScanGeneration;
        string logLastCleanup;

        static string LogBytes(long bytes) {
            if (bytes >= 1073741824) return (bytes / 1073741824.0).ToString("N2") + " GB";
            if (bytes >= 1048576) return (bytes / 1048576.0).ToString("N1") + " MB";
            if (bytes >= 1024) return (bytes / 1024.0).ToString("N1") + " KB";
            return bytes.ToString("N0") + " B";
        }
        Button LogButton(string name, string text, Action action) {
            var button = new Button { Name = name, Text = text, AutoSize = true, Height = 32, Margin = new Padding(5, 0, 0, 0), Padding = new Padding(8, 4, 8, 4) };
            button.Click += delegate { try { action(); } catch (Exception error) { logStatus.Text = error.Message; } }; return button;
        }
        Control BuildLogManagement() {
            var audit = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Alcohol", "V8Trace", "log-management.jsonl");
            logStorage = new LogStorage(ApplicationRoot, audit);
            logRoot.Text = DisplayLocalPath(Path.Combine(ApplicationRoot, "logs"));
            var panel = new TableLayoutPanel { Name = "LogManagementPanel", Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 7, Padding = new Padding(18, 12, 14, 8) };
            panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            foreach (var height in new[] { 42, 30, 43, 30 }) panel.RowStyles.Add(new RowStyle(SizeType.Absolute, height));
            panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 74)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 43));
            panel.Controls.Add(new Label { Text = "日志管理  /  LOG ARCHIVE", Font = new Font("Microsoft YaHei UI", 18F, FontStyle.Bold), Dock = DockStyle.Fill }, 0, 0);
            logSummary.Text = "0 项 · 0 B · 本地原始采集档案"; panel.Controls.Add(logSummary, 0, 1);
            var path = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 4, RowCount = 1, Margin = new Padding(0) };
            path.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            path.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); foreach (var width in new[] { 68, 98, 68 }) path.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, width));
            logRoot.Margin = new Padding(0, 2, 5, 7); path.Controls.Add(logRoot, 0, 0);
            var browse = LogButton("LogBrowseButton", "浏览…", BrowseLogRoot);
            var openRoot = LogButton("LogOpenDirectoryButton", "打开目录", OpenLogRoot);
            var refreshLogs = LogButton("LogRefreshButton", "刷新", RefreshLogs);
            foreach (var control in new[] { browse, openRoot, refreshLogs }) { control.AutoSize = false; control.Padding = new Padding(0); control.Margin = new Padding(5, 0, 0, 6); control.Dock = DockStyle.Fill; logPathControls.Add(control); }
            path.Controls.Add(browse, 1, 0); path.Controls.Add(openRoot, 2, 0); path.Controls.Add(refreshLogs, 3, 0); panel.Controls.Add(path, 0, 2);
            var select = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false, Margin = new Padding(0) };
            select.Controls.Add(logSelectAll); select.Controls.Add(new Label { AutoSize = true, Text = "   当前载入、采集中、MCP 占用及链接项自动保留", Padding = new Padding(0, 3, 0, 0) }); panel.Controls.Add(select, 0, 3);
            var list = new Panel { Dock = DockStyle.Fill, Padding = new Padding(0), Margin = new Padding(0) };
            logGrid.Columns.Add(new DataGridViewCheckBoxColumn { Name = "Selected", HeaderText = "", Width = 32, SortMode = DataGridViewColumnSortMode.NotSortable });
            logGrid.Columns.Add(new DataGridViewTextBoxColumn { Name = "LogName", HeaderText = "名称", AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill, MinimumWidth = 130, FillWeight = 50, ReadOnly = true });
            logGrid.Columns.Add(new DataGridViewTextBoxColumn { Name = "LogType", HeaderText = "类型", Width = 68, ReadOnly = true });
            logGrid.Columns.Add(new DataGridViewTextBoxColumn { Name = "LogSize", HeaderText = "大小", Width = 100, ReadOnly = true });
            logGrid.Columns.Add(new DataGridViewTextBoxColumn { Name = "LogModified", HeaderText = "修改时间", Width = 146, ReadOnly = true });
            logGrid.Columns.Add(new DataGridViewTextBoxColumn { Name = "LogState", HeaderText = "状态", AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill, MinimumWidth = 120, FillWeight = 35, ReadOnly = true });
            logGrid.Columns["LogSize"].DefaultCellStyle.Alignment = DataGridViewContentAlignment.MiddleRight;
            list.Controls.Add(logGrid); list.Controls.Add(logEmpty); panel.Controls.Add(list, 0, 4);
            var operations = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = true, Padding = new Padding(0, 12, 0, 0), Margin = new Padding(0) };
            recycleLogs.Margin = new Padding(0, 9, 14, 0); operations.Controls.Add(recycleLogs);
            foreach (var button in new[] { logClearLarge, logClearSelected, logClearAll, logCancel }) { button.Height = 34; button.Padding = new Padding(10, 6, 10, 6); button.Margin = new Padding(5, 0, 0, 5); operations.Controls.Add(button); }
            panel.Controls.Add(operations, 0, 5); panel.Controls.Add(logStatus, 0, 6);
            logRoot.TextChanged += delegate { if (!logsDeleting) { CancelLogScan(); logSnapshot = null; logLastCleanup = null; logRowsChanging = true; logGrid.Rows.Clear(); logSelectAll.Checked = false; logRowsChanging = false; logSummary.Text = "目录已更改 · 刷新后查看"; logGrid.Visible = false; logEmpty.Visible = true; logEmpty.Text = "目录已更改\n\n点击刷新以读取新目录。"; UpdateLogActions(); } };
            logRoot.KeyDown += delegate(object sender, KeyEventArgs args) { if (args.KeyCode == Keys.Enter) { args.SuppressKeyPress = true; RefreshLogs(); } };
            logSelectAll.CheckedChanged += delegate { if (logRowsChanging) return; logRowsChanging = true; foreach (DataGridViewRow row in logGrid.Rows) if (((LogStorageItem)row.Tag).CanClean) row.Cells[0].Value = logSelectAll.Checked; logRowsChanging = false; UpdateLogActions(); };
            logGrid.CurrentCellDirtyStateChanged += delegate { if (logGrid.IsCurrentCellDirty) logGrid.CommitEdit(DataGridViewDataErrorContexts.Commit); };
            logGrid.CellValueChanged += delegate { if (!logRowsChanging) UpdateLogActions(); };
            logGrid.CellDoubleClick += delegate(object sender, DataGridViewCellEventArgs args) { if (args.RowIndex < 0 || logsDeleting) return; try { var item = (LogStorageItem)logGrid.Rows[args.RowIndex].Tag; if (item.Directory && (File.GetAttributes(item.Path) & FileAttributes.ReparsePoint) == 0) { logRoot.Text = item.Path; RefreshLogs(); } } catch (Exception error) { logStatus.Text = "目录已变化：" + error.Message; } };
            logGrid.KeyDown += delegate(object sender, KeyEventArgs args) { if (args.KeyCode == Keys.F5) { args.Handled = true; RefreshLogs(); } };
            logClearAll.Click += delegate { CleanupLogs("all"); }; logClearSelected.Click += delegate { CleanupLogs("selected"); }; logClearLarge.Click += delegate { CleanupLogs("large"); };
            logCancel.Click += delegate { if (logDeleteCancellation != null) logDeleteCancellation.Cancel(); CancelLogScan(); logStatus.Text = "已请求取消，当前项处理结束后保留其余条目。"; };
            recycleLogs.CheckedChanged += delegate { logStatus.Text = recycleLogs.Checked ? "默认移至回收站；每次清理前都会列明目录与条目。" : "永久删除不经过回收站；清理时需额外输入 DELETE。"; };
            UpdateLogActions(); return panel;
        }
        void StyleLogActions() {
            logClearAll.ForeColor = CyberPalette.Danger; logClearAll.FlatAppearance.BorderColor = CyberPalette.Danger;
            logClearSelected.ForeColor = CyberPalette.Cyan; logStatus.ForeColor = CyberPalette.Muted; logSummary.ForeColor = CyberPalette.Muted; logEmpty.ForeColor = CyberPalette.Muted;
        }
        List<string> ProtectedLogPaths() {
            var paths = new List<string>();
            if (!String.IsNullOrEmpty(captureDirectory)) { paths.Add(captureDirectory); paths.Add(captureDirectory + ".stream"); }
            if (busy && captureRequestId != null) { var active = ResolveLocalPath(output.Text); paths.Add(active); paths.Add(active + ".stream"); }
            if (mcpService.Active) foreach (var input in new[] { mcpSource, mcpIndex, mcpExports, mcpToken }) paths.Add(ResolveLocalPath(input.Text));
            return paths;
        }
        void EnsureLogList() { if (logSnapshot == null && !logsScanning && !logsDeleting) RefreshLogs(); }
        void BrowseLogRoot() {
            if (logsDeleting) return;
            using (var dialog = new FolderBrowserDialog { Description = "选择要管理的日志目录", ShowNewFolderButton = false }) {
                try { dialog.SelectedPath = ResolveLocalPath(logRoot.Text); } catch (Exception) { }
                if (dialog.ShowDialog(this) == DialogResult.OK) { logRoot.Text = dialog.SelectedPath; RefreshLogs(); }
            }
        }
        void OpenLogRoot() {
            var path = ResolveLocalPath(logRoot.Text);
            if (!Directory.Exists(path)) { logStatus.Text = "该目录尚不存在，请浏览选择已有日志目录。"; return; }
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true });
        }
        void CancelLogScan() { ++logScanGeneration; if (logScanCancellation != null) logScanCancellation.Cancel(); logsScanning = false; }
        void OnLogUi(Action action) { try { if (!IsDisposed && IsHandleCreated) BeginInvoke(action); } catch (InvalidOperationException) { } }
        void RefreshLogs() {
            if (logsDeleting) return;
            CancelLogScan();
            string root; try { root = ResolveLocalPath(logRoot.Text); } catch (Exception error) { logStatus.Text = error.Message; return; }
            var protectedPaths = ProtectedLogPaths(); var generation = logScanGeneration;
            logScanCancellation = new CancellationTokenSource(); var token = logScanCancellation.Token;
            logSnapshot = null; logsScanning = true; logStatus.Text = "正在后台统计日志目录，界面保持响应…"; logSummary.Text = "SCANNING / 正在统计"; UpdateLogActions();
            Task.Factory.StartNew(delegate {
                try { var snapshot = logStorage.Scan(root, protectedPaths, token); OnLogUi(delegate { if (generation != logScanGeneration || token.IsCancellationRequested) return; logsScanning = false; logSnapshot = snapshot; RenderLogRows(); }); }
                catch (OperationCanceledException) { }
                catch (Exception error) { OnLogUi(delegate { if (generation != logScanGeneration) return; logsScanning = false; logStatus.Text = "目录读取失败：" + error.Message; logSummary.Text = "0 项 · 未完成统计"; UpdateLogActions(); }); }
            }, token);
        }
        void RenderLogRows() {
            logRowsChanging = true; logGrid.Rows.Clear(); logSelectAll.Checked = false;
            foreach (var item in logSnapshot.Items) {
                var index = logGrid.Rows.Add(false, item.Name, item.Directory ? "目录" : "文件", item.SizeKnown ? LogBytes(item.Bytes) : "—", item.ModifiedUtc == DateTime.MinValue ? "—" : item.ModifiedUtc.ToLocalTime().ToString("yyyy-MM-dd HH:mm"), item.State);
                var row = logGrid.Rows[index]; row.Tag = item; row.Cells[0].ReadOnly = !item.CanClean;
                foreach (DataGridViewCell cell in row.Cells) cell.ToolTipText = item.Path + "\n" + item.State;
                if (!item.CanClean) row.DefaultCellStyle.ForeColor = CyberPalette.Muted;
            }
            logRowsChanging = false;
            logSummary.Text = (logSnapshot.Truncated ? "已列出 " : "共 ") + logSnapshot.Items.Count.ToString("N0") + " 项 · 已统计 " + LogBytes(logSnapshot.KnownBytes) + " · " + logSnapshot.Root;
            logEmpty.Visible = logSnapshot.Items.Count == 0; logGrid.Visible = !logEmpty.Visible;
            logEmpty.Text = String.IsNullOrEmpty(logSnapshot.Warning) ? "ARCHIVE EMPTY / 暂无日志\n\n完成采集后，可在此浏览、打开与清理历史记录。" : "LOG DIRECTORY / 目录提示\n\n" + logSnapshot.Warning;
            logStatus.Text = !String.IsNullOrEmpty(logSnapshot.Warning) ? logSnapshot.Warning : (logLastCleanup ?? "后台统计完成 · 使用中、受保护或内容变化的条目保持原样。");
            UpdateLogActions();
        }
        List<LogStorageItem> CheckedLogs() {
            return logGrid.Rows.Cast<DataGridViewRow>().Where(row => row.Tag is LogStorageItem && ((LogStorageItem)row.Tag).CanClean && Convert.ToBoolean(row.Cells[0].Value ?? false)).Select(row => (LogStorageItem)row.Tag).ToList();
        }
        void UpdateLogActions() {
            var idle = !logsScanning && !logsDeleting && !busy && !closing; var items = logSnapshot == null ? new List<LogStorageItem>() : logSnapshot.Items.Where(x => x.CanClean).ToList();
            var selected = CheckedLogs().Count; logClearSelected.Text = "清理选中 (" + selected + ")";
            logClearSelected.Enabled = idle && logSnapshot != null && selected > 0; logClearAll.Enabled = idle && logSnapshot != null && !logSnapshot.Truncated && items.Count > 0;
            logClearLarge.Enabled = idle && logSnapshot != null && !logSnapshot.Truncated && items.Any(x => x.SizeKnown && x.Bytes > LogStorage.LargeBytes);
            logSelectAll.Enabled = idle && items.Count > 0; recycleLogs.Enabled = idle;
            logRoot.Enabled = !logsDeleting; foreach (var control in logPathControls) control.Enabled = !logsDeleting;
            logCancel.Visible = logsScanning || logsDeleting;
            McpControls();
            if (logsDeleting) { start.Enabled = false; load.Enabled = false; foreach (var button in workspaceNavigation.Values) button.Enabled = false; }
            else { foreach (var button in workspaceNavigation.Values) button.Enabled = !closing; start.Enabled = ready && !busy && !closing; load.Enabled = !busy && !closing; }
        }
        bool ConfirmLogCleanup(int count, long bytes, bool recycle) {
            using (var dialog = new Form { Name = "LogCleanupConfirmation", Text = "确认日志清理 · Alcohol", StartPosition = FormStartPosition.CenterParent, Size = new Size(620, 330), MinimumSize = new Size(620, 330), FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false, ShowInTaskbar = false, Icon = Icon }) {
                var content = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5, Padding = new Padding(20) };
                foreach (var height in new[] { 38, 74, 44, 36, 48 }) content.RowStyles.Add(new RowStyle(SizeType.Absolute, height));
                content.Controls.Add(new Label { Text = (recycle ? "移至回收站" : "永久删除") + " · " + count + " 项 · " + LogBytes(bytes), Dock = DockStyle.Fill, Font = new Font("Microsoft YaHei UI", 13F, FontStyle.Bold) }, 0, 0);
                content.Controls.Add(new TextBox { Multiline = true, ReadOnly = true, Text = logSnapshot.Root, Dock = DockStyle.Fill, WordWrap = true }, 0, 1);
                content.Controls.Add(new Label { Text = recycle ? "仅处理当前目录的已列出条目。使用中、链接、工程数据与变化项保留。" : "永久删除不经过回收站。输入 DELETE 确认；保护规则仍然生效。", Dock = DockStyle.Fill }, 0, 2);
                var keyword = new TextBox { Name = "LogPermanentConfirmation", Dock = DockStyle.Fill, Visible = !recycle }; content.Controls.Add(keyword, 0, 3);
                var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.RightToLeft };
                var confirm = new Button { Name = "LogConfirmCleanup", Text = recycle ? "确认移至回收站" : "确认永久删除", DialogResult = DialogResult.OK, Width = 160, Height = 34, Enabled = recycle };
                var cancel = new Button { Text = "取消", DialogResult = DialogResult.Cancel, Width = 90, Height = 34 };
                keyword.TextChanged += delegate { confirm.Enabled = keyword.Text == "DELETE"; }; actions.Controls.Add(confirm); actions.Controls.Add(cancel); content.Controls.Add(actions, 0, 4); dialog.Controls.Add(content);
                CyberPalette.Apply(dialog); confirm.ForeColor = recycle ? CyberPalette.Cyan : CyberPalette.Pink; dialog.AcceptButton = confirm; dialog.CancelButton = cancel;
                return dialog.ShowDialog(this) == DialogResult.OK;
            }
        }
        void CleanupLogs(string mode) {
            if (logsScanning || logsDeleting || busy || closing || logSnapshot == null) return;
            var items = mode == "selected" ? CheckedLogs() : logSnapshot.Items.Where(x => x.CanClean && (mode != "large" || x.Bytes > LogStorage.LargeBytes)).ToList();
            if (mode != "selected" && logSnapshot.Truncated || items.Count == 0) return;
            var recycle = recycleLogs.Checked;
            if (!ConfirmLogCleanup(items.Count, items.Sum(x => x.Bytes), recycle)) return;
            var snapshot = logSnapshot; var protectedPaths = ProtectedLogPaths();
            logDeleteCancellation = new CancellationTokenSource(); var token = logDeleteCancellation.Token; logsDeleting = true; logStatus.Text = "正在处理已确认的日志条目…"; UpdateLogActions();
            var thread = new Thread(new ThreadStart(delegate {
                try {
                    var result = logStorage.Clean(snapshot, items.Select(x => x.Path), protectedPaths, recycle, token);
                    OnLogUi(delegate { logsDeleting = false; logDeleteCancellation.Dispose(); logDeleteCancellation = null; logLastCleanup = "完成 " + result.Removed + " · 保留 " + result.Skipped + " · 失败 " + result.Failed + (result.Cancelled ? " · 已取消" : "") + (result.Details.Count == 0 ? "" : " · " + result.Details[0]); UpdateLogActions(); RefreshLogs(); logStatus.Text = logLastCleanup; });
                } catch (Exception error) { OnLogUi(delegate { logsDeleting = false; logDeleteCancellation.Dispose(); logDeleteCancellation = null; UpdateLogActions(); logStatus.Text = "清理未完成：" + error.Message; }); }
            })) { IsBackground = true, Name = "V8Trace log cleanup" };
            thread.SetApartmentState(ApartmentState.STA); thread.Start();
        }
        bool StopLogCleanupForClose(FormClosingEventArgs args) {
            if (!logsDeleting) { CancelLogScan(); return false; }
            if (logDeleteCancellation != null) logDeleteCancellation.Cancel(); args.Cancel = true;
            logStatus.Text = "已请求停止清理；当前项处理结束后，可再次关闭窗口。"; return true;
        }
    }
}
