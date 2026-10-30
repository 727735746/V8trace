using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;

namespace V8Trace {
    public sealed partial class TraceWindow {
        readonly RichTextBox configurationText = new RichTextBox { Name = "ConfigurationJson", Dock = DockStyle.Fill, WordWrap = false, Font = new Font("Consolas", 9F), MaxLength = 1048576 };
        readonly Label configurationStatus = new Label { Name = "ConfigurationStatus", Dock = DockStyle.Fill, AutoEllipsis = true };
        readonly List<Button> studioActions = new List<Button>();
        readonly List<Button> guidedSetupActions = new List<Button>();
        readonly Label guidedStatus = new Label { Name = "GuidedWorkflowStatus", AutoEllipsis = true, Text = "引导填写参数 → 预检 → 手动开始 → 查看证据 / 导出。下方手动设置保持可用。" };
        readonly Button guidedInspect = new Button { Name = "GuidedInspectButton", Text = "查看摘要 / 缺口", AutoSize = true, Enabled = false };
        readonly Button guidedExport = new Button { Name = "GuidedExportButton", Text = "导出案例", AutoSize = true, Enabled = false };
        bool guidedPreflightPending;
        readonly Button sharingCopy = new Button { Name = "SharingCopyButton", Text = "分享副本 / 字段设置", AutoSize = true, Enabled = false };
        bool sharingAvailable;
        ShareCopyView shareView;
        Dictionary<string, object> expectedLinkedRow;
        readonly ComboBox gapFindings = new ComboBox { Name = "GapFindings", Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList, DropDownWidth = 650 };
        readonly RichTextBox gapDiagnosticText = new RichTextBox { Name = "GapDiagnosticText", Dock = DockStyle.Fill, ReadOnly = true, WordWrap = false, Font = new Font("Consolas", 9F) };
        readonly Label viewerStatus = new Label { Name = "IndexStatus", Dock = DockStyle.Fill, AutoEllipsis = true };
        readonly DataGridView indexGrid = new DataGridView { Name = "IndexGrid", Dock = DockStyle.Fill, ReadOnly = true, AllowUserToAddRows = false, AllowUserToDeleteRows = false, RowHeadersVisible = false, SelectionMode = DataGridViewSelectionMode.FullRowSelect, MultiSelect = false, AutoSizeRowsMode = DataGridViewAutoSizeRowsMode.None, BackgroundColor = Color.White };
        readonly RichTextBox indexedText = new RichTextBox { Name = "IndexedDetails", Dock = DockStyle.Fill, ReadOnly = true, WordWrap = false, Font = new Font("Consolas", 9F) };
        readonly Dictionary<string, TextBox> indexFilters = new Dictionary<string, TextBox>();
        readonly ComboBox indexedBlobs = new ComboBox { Name = "IndexedBlobReferences", DropDownStyle = ComboBoxStyle.DropDownList, Width = 230, DropDownWidth = 500 };
        readonly ComboBox indexedCategory = new ComboBox { Name = "IndexCategoryInput", Width = 120, DropDownStyle = ComboBoxStyle.DropDown };
        readonly Button indexNext = new Button { Name = "IndexNextButton", Text = "下一页", AutoSize = true };
        readonly Button valueNext = new Button { Name = "ValueNextButton", Text = "后一段", AutoSize = true };
        readonly Button valuePrevious = new Button { Name = "ValuePreviousButton", Text = "前一段", AutoSize = true };
        Dictionary<string, object> importedOptions, importedSnapshot, pendingConfiguration, studioPreflightOptions, viewerSelection;
        string studioRequestId, studioRequestKind, viewerGeneration, viewerCursor = "0";
        bool studioAvailable, startAfterPreflight, indexHasMore, valueHasMore, renderingIndex;
        long valueOffset, valueNextOffset;

        Button StudioButton(string name, string text, Action action) {
            var button = new Button { Name = name, Text = text, AutoSize = true, Height = 28 };
            button.Click += (sender, args) => { try { action(); } catch (Exception error) { ShowError(error.Message); } };
            studioActions.Add(button); return button;
        }
        void BuildGuidedWorkflow(TableLayoutPanel settings) {
            Row(settings, new Label { Text = "快速采集 · 确认应用后仅预检，不自动采集", TextAlign = ContentAlignment.BottomLeft }, 28);
            var routes = new TableLayoutPanel { ColumnCount = 2, Margin = new Padding(0) };
            routes.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50)); routes.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            foreach (var entry in new[] { new[] { "env-protocol", "GuidedEnvironmentButton", "补环境 + 协议（引导）" }, new[] { "env-target-detail", "GuidedDetailButton", "目标调用详情（引导）" } }) {
                var id = entry[0]; var button = new Button { Name = entry[1], Text = entry[2], Dock = DockStyle.Fill, Enabled = false, Margin = new Padding(0, 0, 4, 0) };
                button.Click += delegate { try { BeginGuidedSetup(id); } catch (Exception error) { ShowError(error.Message); } };
                guidedSetupActions.Add(button); routes.Controls.Add(button);
            }
            Row(settings, routes, 36); Row(settings, guidedStatus, 58);
            var evidence = new FlowLayoutPanel { Margin = new Padding(0), WrapContents = false };
            evidence.Controls.Add(guidedInspect); evidence.Controls.Add(guidedExport); Row(settings, evidence, 34); Row(settings, sharingCopy, 32);
            guidedInspect.Click += delegate { ShowEvidenceSummary(); };
            guidedExport.Click += delegate { ExportCapture(); };
            sharingCopy.Click += delegate { try { ShowSharingCopy(); } catch (Exception error) { ShowError(error.Message); } };
        }
        void UpdateGuidedActions() {
            bool idle = ready && !busy && !closing && !logsDeleting;
            foreach (var button in guidedSetupActions) button.Enabled = idle;
            guidedInspect.Enabled = guidedExport.Enabled = idle && captureDirectory != null;
            sharingCopy.Enabled = idle && sharingAvailable && captureDirectory != null;
        }
        void ShowEvidenceSummary() {
            workspaces.SelectedIndex = 0;
            var tabs = FindCyberControl(this, "EvidenceTabs") as TabControl;
            if (tabs != null) tabs.SelectedIndex = 1;
        }
        void BeginGuidedSetup(string id) { ShowGuidedSetup(id, null, null); }
        sealed class ShareCopyView {
            public readonly Form Window;
            public readonly TextBox Source = new TextBox { Name = "ShareSourceInput", AccessibleName = "分享来源目录" };
            public readonly TextBox Output = new TextBox { Name = "ShareOutputInput", AccessibleName = "独立分享副本新目录" };
            public readonly RichTextBox Rules = new RichTextBox { Name = "ShareFieldSettings", Dock = DockStyle.Fill, WordWrap = false, MaxLength = 65536, AccessibleName = "字段设置 JSON", Font = new Font("Consolas", 9F) };
            public readonly RichTextBox Preview = new RichTextBox { Name = "ShareFieldPreview", Dock = DockStyle.Fill, ReadOnly = true, WordWrap = false, AccessibleName = "实际字段规则与覆盖预览", Font = new Font("Consolas", 9F) };
            public readonly Label Status = new Label { Name = "ShareCopyStatus", Dock = DockStyle.Fill, AutoEllipsis = true };
            public readonly Button Inspect = new Button { Name = "SharePreviewButton", Text = "预览字段 / 覆盖", AutoSize = true };
            public readonly Button Create = new Button { Name = "ShareCreateButton", Text = "确认创建分享副本", AutoSize = true, Enabled = false };
            public readonly Button Cancel = new Button { Name = "ShareCancelTaskButton", Text = "取消任务", AutoSize = true, Enabled = false };
            public Dictionary<string, object> Review;
            public bool CloseWhenIdle;
            public ShareCopyView(TraceWindow owner) {
                Window = new Form { Name = "SharingCopyDialog", Text = "分享副本 / 字段设置 · " + owner.editionLabel, Size = new Size(1000, 760), MinimumSize = new Size(820, 680), StartPosition = FormStartPosition.CenterParent, Icon = owner.Icon, Font = owner.Font, MinimizeBox = false };
            }
            public void UpdateBusy(bool busy) { Source.Enabled = Output.Enabled = Rules.Enabled = Inspect.Enabled = !busy; Cancel.Enabled = busy; Create.Enabled = !busy && Review != null; }
        }
        void ShowSharingCopy() {
            if (!sharingAvailable || !ready || busy || closing || logsDeleting || captureDirectory == null || shareView != null) return;
            var view = new ShareCopyView(this); shareView = view;
            using (view.Window) {
                try {
                    view.Source.Text = captureDirectory; view.Output.Text = Path.Combine(Path.GetDirectoryName(ResolveLocalPath(captureDirectory)), "sharing-copy-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff"));
                    view.Rules.Text = Pretty(new Dictionary<string, object> { { "protocol", "v8trace/share-fields/1" }, { "copyOpaque", false }, { "rules", new[] {
                        new { field = "authorization", action = "replace", value = "[字段已处理]" }, new { field = "cookie", action = "replace", value = "[字段已处理]" },
                        new { field = "set-cookie", action = "replace", value = "[字段已处理]" }, new { field = "password", action = "replace", value = "[字段已处理]" }, new { field = "token", action = "replace", value = "[字段已处理]" } } } });
                    var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 4, Padding = new Padding(12) };
                    layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 200)); layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 28)); layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
                    var inputs = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, AutoScroll = true };
                    Field(inputs, "来源：已完成采集 / 原始导出（只读）", view.Source, () => { if (busy) return; using (var folder = new FolderBrowserDialog()) if (folder.ShowDialog(view.Window) == DialogResult.OK) view.Source.Text = folder.SelectedPath; });
                    Field(inputs, "独立新目录：保留父目录，不覆盖已存在目录", view.Output, () => { if (busy) return; using (var folder = new FolderBrowserDialog()) if (folder.ShowDialog(view.Window) == DialogResult.OK) view.Output.Text = Path.Combine(folder.SelectedPath, "sharing-copy-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff")); });
                    Row(inputs, new Label { Text = "只处理 JSON / JSONL 的精确字段名（ASCII 大小写归一），不解析字符串内编码内容。默认省略二进制、源码等非 JSON 文件；copyOpaque=true 仅原样复制，不处理其字段。原路径 / 原哈希仍写入清单，引用不修复；请逐项核对覆盖边界。副本不是原始完整采集或已验证复现。", AutoEllipsis = true }, 76); layout.Controls.Add(inputs, 0, 0);
                    view.Status.Text = "先预览实际命中字段、规则与省略范围；确认后只写独立新目录。"; layout.Controls.Add(view.Status, 0, 1);
                    var panes = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 2 };
                    panes.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 40)); panes.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 60)); panes.RowStyles.Add(new RowStyle(SizeType.Absolute, 24)); panes.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
                    panes.Controls.Add(new Label { Text = "字段设置（replace / remove）", Dock = DockStyle.Fill }, 0, 0); panes.Controls.Add(new Label { Text = "内核预览 / 清单回执（不显示原字段值）", Dock = DockStyle.Fill }, 1, 0);
                    panes.Controls.Add(view.Rules, 0, 1); panes.Controls.Add(view.Preview, 1, 1); layout.Controls.Add(panes, 0, 2);
                    var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false }; var close = new Button { Name = "ShareCloseButton", Text = "关闭", AutoSize = true };
                    actions.Controls.Add(view.Inspect); actions.Controls.Add(view.Create); actions.Controls.Add(view.Cancel); actions.Controls.Add(close); layout.Controls.Add(actions, 0, 3); view.Window.Controls.Add(layout);
                    Action invalidate = delegate { view.Review = null; view.Create.Enabled = false; view.Status.Text = "参数已变化；请重新预览，原始文件未处理。"; };
                    view.Source.TextChanged += delegate { invalidate(); }; view.Output.TextChanged += delegate { invalidate(); }; view.Rules.TextChanged += delegate { invalidate(); };
                    view.Inspect.Click += delegate { RequestSharingCopy(false); }; view.Create.Click += delegate { RequestSharingCopy(true); };
                    view.Cancel.Click += delegate { view.Status.Text = "正在请求取消；已写入新目录的部分文件保留，不标为完成。"; Send("stop"); };
                    close.Click += delegate { view.Window.Close(); };
                    view.Window.FormClosing += (sender, args) => { if (busy && studioRequestKind != null && studioRequestKind.StartsWith("share-", StringComparison.Ordinal)) { args.Cancel = true; view.CloseWhenIdle = true; view.Cancel.PerformClick(); } };
                    CyberPalette.Apply(view.Window); view.Window.ShowDialog(this);
                } finally { shareView = null; }
            }
        }
        void RequestSharingCopy(bool create) {
            var view = shareView; if (view == null || busy || !ready || closing || logsDeleting) return;
            try {
                if (create && view.Review == null) throw new InvalidOperationException("请先预览当前字段设置及来源快照。");
                var fields = new Dictionary<string, object> { { "source", ResolveLocalPath(view.Source.Text) }, { "settings", json.DeserializeObject(view.Rules.Text) } };
                if (create) { fields["output"] = ResolveLocalPath(view.Output.Text); fields["previewBinding"] = Value(view.Review, "previewBinding"); }
                view.Status.Text = create ? "核对预览快照并创建独立分享副本；原始目录只读。" : "正在逐文件核对字段与覆盖边界，尚未创建副本。";
                StudioSend(create ? "share-copy" : "share-preview", fields, view.Status.Text); stop.Enabled = true;
            } catch (Exception error) { view.Review = null; view.UpdateBusy(false); view.Status.Text = "任务未提交：" + error.Message; }
        }
        bool SharingMessage(Dictionary<string, object> message) {
            var type = Value(message, "type"); if (type != "share-progress" && type != "share-preview" && type != "share-copy") return false;
            if (Value(message, "id") != studioRequestId) return true;
            var view = shareView;
            if (type == "share-progress") { if (view != null) view.Status.Text = "核对 / 复制：" + Value(message, "source"); return true; }
            studioRequestId = studioRequestKind = null; SetBusy(false);
            if (view != null) {
                view.Review = type == "share-preview" ? CloneOptions(message) : null; view.Preview.Text = Pretty(message);
                view.Status.Text = type == "share-preview" ? "预览已就绪；检查规则、命中字段、省略与覆盖边界后，明确确认创建。" : "分享副本已创建；以清单核对字段覆盖和原始 / 副本哈希，不代表全内容均已处理。";
                view.UpdateBusy(false); if (view.CloseWhenIdle) view.Window.Close();
            }
            return true;
        }
        void ShowGuidedSetup(string id, string proposedScope, Dictionary<string, object> origin) {
            if (!ready || busy || closing || logsDeleting) return;
            var choice = preset.Items.OfType<ScenarioChoice>().SingleOrDefault(item => Value(item.Record, "id") == id);
            if (choice == null) throw new InvalidOperationException("当前内核没有此预设，请一起更新内核与资产。");
            if (!PresetAvailable(choice)) { guidedStatus.Text = "此预设被当前编译版本限制；下方预设说明保留具体原因。当前配置未变。"; return; }
            bool scoped = RequiresScriptScope(choice);
            using (var headingFont = new Font(Font, FontStyle.Bold))
            using (var dialog = new Form { Name = "GuidedCaptureDialog", Text = "快速采集参数 · " + editionLabel, StartPosition = FormStartPosition.CenterParent,
                Size = new Size(780, 730), MinimumSize = new Size(680, 650), MinimizeBox = false, MaximizeBox = false, Icon = Icon, Font = Font }) {
                var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, AutoScroll = true, Padding = new Padding(18) };
                var targetUrl = new TextBox { Name = "GuidedUrlInput", AccessibleName = "目标 URL", Text = url.Text };
                var targetChrome = new TextBox { Name = "GuidedChromeInput", AccessibleName = "配套 Chrome 可执行文件", Text = chrome.Text };
                var targetOutput = new TextBox { Name = "GuidedOutputInput", AccessibleName = "新日志输出目录", Text = output.Text };
                var targetScope = new TextBox { Name = "GuidedScopeInput", AccessibleName = "具体目标脚本范围", Text = proposedScope ?? presetScriptScope.Text, Multiline = true, ScrollBars = ScrollBars.Vertical, MaxLength = 8192, Enabled = scoped };
                Row(panel, new Label { Text = choice.ToString(), Font = headingFont }, 30);
                var options = ObjectValue(choice.Record, "options");
                var preview = "阶段：" + Value(choice.Record, "stage") + " / 开销：" + Value(choice.Record, "risk") +
                    "；预算 " + Value(options, "durationSeconds") + " 秒 / " + Value(options, "maxEvents") + " 事件 / " + Value(options, "maxBytes") + " 字节；正文 " + Value(options, "maxBodyBytes") + " 字节。\n" +
                    "详情范围见下方；Env / HTTP / 源码不受此范围限制。Opcode=" + Value(options, "opcode") + " / VM探针=" + Value(options, "vmpProbe") + "。实际生效契约由确认后的预检列明。\n\n" + Value(choice.Record, "purpose") + "\n要求：" + Value(choice.Record, "requirement");
                if (origin != null) preview += "\n\n来自原始证据（不是算法或 VM 语义确认）：\n" + Pretty(origin) + "\n范围可编辑；含通配字符的 URL 不预填为 glob。当前采集包不改写，新输出需显式选择。";
                Row(panel, new RichTextBox { Name = "GuidedPresetDescription", Text = preview,
                    ReadOnly = true, BorderStyle = BorderStyle.None, WordWrap = true }, 100);
                Field(panel, "目标 URL", targetUrl);
                Field(panel, "配套 Chrome 可执行文件（以预检校验实际能力）", targetChrome, () => {
                    using (var browser = new OpenFileDialog { Filter = "Chrome 程序|*.exe" }) if (browser.ShowDialog(dialog) == DialogResult.OK) targetChrome.Text = DisplayLocalPath(browser.FileName);
                });
                Field(panel, "日志输出（新目录；相对路径基于软件目录）", targetOutput, () => {
                    using (var folder = new FolderBrowserDialog { Description = "选择日志根目录，将创建新的采集子目录" }) if (folder.ShowDialog(dialog) == DialogResult.OK)
                        targetOutput.Text = DisplayLocalPath(Path.Combine(folder.SelectedPath, "capture-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff")));
                });
                Row(panel, new Label { Text = scoped ? "具体目标脚本 URL / 文件名 glob（每行一条，排除全局通配）" : "此组合不启用密集调用详情；已有目标范围保留供定向组合使用。" }, 36);
                Row(panel, targetScope, 60);
                Row(panel, new Label { Text = "目标范围仅约束 JSCall/Native 等已选调用详情，不约束 Env Hook、HTTP 或源码。确认应用会按原预设覆盖通道、过滤和预算，未呈现的导入字段不继承；无头、随停随开等运行选择保留。只预检，不启动浏览器。", AutoEllipsis = true }, 80);
                var validation = new Label { Name = "GuidedValidation", AutoEllipsis = true }; Row(panel, validation, 38);
                var apply = new Button { Name = "GuidedApplyButton", Text = "确认应用 → 预检", AutoSize = true, DialogResult = DialogResult.OK };
                var cancel = new Button { Name = "GuidedCancelButton", Text = "取消", AutoSize = true, DialogResult = DialogResult.Cancel };
                var actions = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 46, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(8) };
                actions.Controls.Add(cancel); actions.Controls.Add(apply); dialog.Controls.Add(panel); dialog.Controls.Add(actions); dialog.AcceptButton = apply; dialog.CancelButton = cancel;
                Action validate = delegate {
                    bool basic = !String.IsNullOrWhiteSpace(targetUrl.Text) && !String.IsNullOrWhiteSpace(targetChrome.Text) && !String.IsNullOrWhiteSpace(targetOutput.Text);
                    bool scope = !scoped || HasConcreteScriptScope(targetScope.Text);
                    apply.Enabled = basic && scope; validation.Text = !scope ? "请填写具体脚本范围；空范围及仅含 * / ? 的规则不会应用。" : !basic ? "请填写 URL、浏览器与新输出目录；实际能力由内核预检。" : "确认后只应用参数并预检；仍需手动点击开始采集。";
                };
                foreach (var input in new[] { targetUrl, targetChrome, targetOutput, targetScope }) input.TextChanged += delegate { validate(); };
                dialog.Shown += delegate { if (scoped) targetScope.Focus(); else targetUrl.Focus(); };
                CyberPalette.Apply(dialog); validate();
                if (dialog.ShowDialog(this) != DialogResult.OK) { guidedStatus.Text = "已取消引导，原配置保持不变；手动设置仍可使用。"; return; }
                if (!ready || busy || closing || logsDeleting) { guidedStatus.Text = "内核或任务状态已变化，本次参数未应用。"; return; }
                preset.SelectedItem = choice;
                if (scoped) presetScriptScope.Text = targetScope.Text.Trim();
                ApplyPreset();
                if (activePreset != id) throw new InvalidOperationException("预设没有应用成功；请核对当前选项。");
                url.Text = targetUrl.Text.Trim(); chrome.Text = targetChrome.Text.Trim(); output.Text = targetOutput.Text.Trim();
            }
            guidedPreflightPending = true; guidedStatus.Text = "正在校验实际版本、能力、范围和预算；尚未启动采集。"; ShowEvidenceSummary();
            try { RequestPreflight(CaptureOptions(), false); } catch { guidedPreflightPending = false; throw; }
        }
        Control BuildDiagnosticsPanel() {
            var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 4 };
            panel.RowStyles.Add(new RowStyle(SizeType.Percent, 55)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 38)); panel.RowStyles.Add(new RowStyle(SizeType.Percent, 45));
            panel.Controls.Add(summaryText, 0, 0); panel.Controls.Add(gapFindings, 0, 1);
            var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
            actions.Controls.Add(StudioButton("OpenGapEvidenceButton", "全部原始缺口", OpenGapEvidence));
            actions.Controls.Add(StudioButton("CopyRecaptureOptionsButton", "复制重采参数补丁", CopyRecaptureOptions));
            panel.Controls.Add(actions, 0, 2); panel.Controls.Add(gapDiagnosticText, 0, 3);
            gapFindings.SelectedIndexChanged += delegate {
                var selected = gapFindings.SelectedItem as ScenarioChoice;
                gapDiagnosticText.Text = selected == null ? "" : Pretty(selected.Record) + "\n\n建议仅为参数补丁；请与原采集 capture-options.json 核对，并选择新输出目录。不会自动应用或启动浏览器。";
            };
            return panel;
        }
        void ShowCaptureDiagnostics(Dictionary<string, object> summary) {
            gapFindings.Items.Clear(); gapDiagnosticText.Clear();
            var diagnostics = ObjectValue(summary, "diagnostics"); object rows;
            if (diagnostics != null && diagnostics.TryGetValue("findings", out rows)) foreach (var value in (IEnumerable)rows) {
                var row = value as Dictionary<string, object>; if (row == null) continue;
                var record = CloneOptions(row); record["label"] = "#" + Value(row, "gapSeq") + " · " + Value(row, "reason") + " · " + Value(row, "source") + " / " + Value(row, "stage");
                gapFindings.Items.Add(new ScenarioChoice { Record = record });
            }
            if (gapFindings.Items.Count > 0) gapFindings.SelectedIndex = 0;
            else gapDiagnosticText.Text = diagnostics == null ? "旧格式未包含结构化诊断；通过“全部原始缺口”核对实际证据，不猜测失败来源。" : "摘要没有列出的缺口；完整性与采集结论仍以原始摘要为准。";
        }
        Dictionary<string, object> RecaptureOptionsDocument() {
            var selected = gapFindings.SelectedItem as ScenarioChoice;
            if (selected == null) throw new InvalidOperationException("请先选中一条缺口诊断。");
            var changes = ObjectValue(selected.Record, "suggestedOptions");
            if (changes == null || changes.Count == 0) throw new InvalidOperationException("此缺口没有已知参数建议；请先核对原始失败记录。");
            return new Dictionary<string, object> { { "protocol", "v8trace/config/1" }, { "schemaVersion", 1 },
                { "pathBase", "application-working-directory" }, { "options", changes }, { "sourceCapture", captureDirectory },
                { "sourceGapSeq", selected.Record["gapSeq"] }, { "usage", "merge patch only; review source capture-options.json and choose a new output" } };
        }
        void CopyRecaptureOptions() {
            Clipboard.SetText(Pretty(RecaptureOptionsDocument()));
            footer.Text = "已复制参数补丁，尚未应用。导入时选“合并”并核对差异、原采集配置和新输出目录。";
        }
        void OpenGapEvidence() {
            if (captureDirectory == null) throw new InvalidOperationException("请先载入或完成一个采集包。");
            foreach (var input in indexFilters.Values) input.Clear(); indexedCategory.Text = "gaps";
            for (Control parent = indexGrid; parent != null; parent = parent.Parent) {
                var page = parent as TabPage;
                if (page != null) { ((TabControl)page.Parent).SelectedTab = page; break; }
            }
            BuildIndex();
        }
        Dictionary<string, object> CloneOptions(Dictionary<string, object> options) { return json.Deserialize<Dictionary<string, object>>(json.Serialize(options)); }
        bool EqualOption(object left, object right) { return json.Serialize(left) == json.Serialize(right); }
        Dictionary<string, object> EffectiveOptions(bool portable) {
            var visible = VisibleOptions(); var result = importedOptions == null ? new Dictionary<string, object>() : CloneOptions(importedOptions);
            var paths = new Dictionary<string, string> { { "output", output.Text }, { "chromePath", chrome.Text }, { "vmpProbeFile", vmpProbeFile.Text }, { "streamTokenFile", tcpToken.Text } };
            foreach (var entry in visible) {
                object baseline;
                if (importedSnapshot != null && importedSnapshot.TryGetValue(entry.Key, out baseline) && EqualOption(baseline, entry.Value)) continue;
                result[entry.Key] = portable && paths.ContainsKey(entry.Key) ? paths[entry.Key] : entry.Value;
            }
            return result;
        }
        Dictionary<string, object> CaptureOptions() { return EffectiveOptions(false); }
        Dictionary<string, object> PortableOptions() { return EffectiveOptions(true); }
        object ConfigurationDocument(Dictionary<string, object> options) { return new Dictionary<string, object> { { "protocol", "v8trace/config/1" }, { "schemaVersion", 1 }, { "pathBase", "application-working-directory" }, { "options", options } }; }

        void BuildConfigurationTab() {
            var tab = new TabPage("配置 / 启动预检") { Name = "ConfigurationTab", BackColor = Color.White };
            var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5, Padding = new Padding(8) };
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 68)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 64)); panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 36)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 46));
            panel.Controls.Add(new Label { Dock = DockStyle.Fill, Text = "UTF-8 配置：导入或编辑后先预览，再确认应用。\n合并递归覆盖对象，数组整体替换；取消不动当前配置。路径保留原文，以软件目录解析。", ForeColor = ink }, 0, 0);
            var actions = new FlowLayoutPanel { Dock = DockStyle.Fill };
            actions.Controls.Add(StudioButton("ConfigCurrentButton", "读取当前", () => { configurationText.Text = Pretty(ConfigurationDocument(PortableOptions())); configurationStatus.Text = "已读取当前配置；编辑稿尚未应用。"; }));
            actions.Controls.Add(StudioButton("ConfigImportButton", "导入替换…", () => ImportConfiguration("replace")));
            actions.Controls.Add(StudioButton("ConfigMergeButton", "导入合并…", () => ImportConfiguration("merge")));
            actions.Controls.Add(StudioButton("ConfigReviewButton", "预览 / 应用", () => PreviewConfiguration(json.Deserialize<Dictionary<string, object>>(configurationText.Text), "replace")));
            actions.Controls.Add(StudioButton("ConfigExportButton", "导出当前…", ExportConfiguration));
            panel.Controls.Add(actions, 0, 1); panel.Controls.Add(configurationText, 0, 2);
            panel.Controls.Add(StudioButton("PreflightButton", "预检当前配置（不启动浏览器）", () => RequestPreflight(CaptureOptions(), false)), 0, 3);
            configurationStatus.Text = "支持范围与路径以右侧预检报告为准；未知字段保留但不执行。"; panel.Controls.Add(configurationStatus, 0, 4); tab.Controls.Add(panel); config.TabPages.Add(tab);
        }
        void ImportConfiguration(string mode) {
            using (var dialog = new OpenFileDialog { Filter = "UTF-8 采集配置|*.json", Title = "选择配置（先预览，不直接应用）" }) if (dialog.ShowDialog(this) == DialogResult.OK) {
                if (new FileInfo(dialog.FileName).Length > 1048576) throw new InvalidOperationException("配置文件超过 1 MiB。");
                PreviewConfiguration(json.Deserialize<Dictionary<string, object>>(File.ReadAllText(dialog.FileName, new UTF8Encoding(false, true))), mode);
            }
        }
        void PreviewConfiguration(Dictionary<string, object> incoming, string mode) {
            StudioSend("config-preview", new Dictionary<string, object> { { "current", PortableOptions() }, { "incoming", incoming }, { "mode", mode } }, "正在计算配置差异…");
        }
        void ExportConfiguration() {
            using (var dialog = new SaveFileDialog { Filter = "UTF-8 采集配置|*.json", FileName = "采集配置.json", OverwritePrompt = true }) if (dialog.ShowDialog(this) == DialogResult.OK) {
                File.WriteAllText(dialog.FileName, Pretty(ConfigurationDocument(PortableOptions())) + "\n", new UTF8Encoding(false)); configurationStatus.Text = "当前已应用配置已完整导出；编辑稿未自动应用。";
            }
        }
        Dictionary<string, NumericUpDown> StudioNumbers() {
            var result = PresetNumbers(); result["closureScopes"] = closureScopes; result["closureBindingsLimit"] = closureBindingsLimit; result["closureDepth"] = closureDepth; result["closureNodes"] = closureNodes; result["closureProperties"] = closureProperties;
            result["envInventoryProperties"] = envInventoryProperties; result["vmpDepth"] = vmpDepth; result["vmpNodes"] = vmpNodes; result["vmpProperties"] = vmpProperties; result["vmpTypedArrayBytes"] = vmpTypedMiB; result["vmpMaxActive"] = vmpMaxActive; result["streamPort"] = tcpPort; return result;
        }
        void ApplyConfigurationDocument(Dictionary<string, object> rawOptions, Dictionary<string, object> displayPlan) {
            var options = displayPlan ?? rawOptions;
            var previous = CaptureOptions(); var savedBase = importedOptions; var savedSnapshot = importedSnapshot;
            try { ApplyConfigurationControls(options); importedOptions = CloneOptions(rawOptions); importedSnapshot = VisibleOptions(); }
            catch { importedOptions = null; importedSnapshot = null; ApplyConfigurationControls(previous); importedOptions = savedBase; importedSnapshot = savedSnapshot; throw; }
        }
        void ApplyConfigurationControls(Dictionary<string, object> options) {
            applyingPreset = true;
            try {
                activeTransport = Value(options, "transport") == "cdp" ? "cdp" : "native";
                changingTransport = true; transport.SelectedIndex = activeTransport == "native" ? 1 : 0; changingTransport = false;
                foreach (var entry in channels) entry.Value.Checked = options.ContainsKey(entry.Key) && Convert.ToBoolean(options[entry.Key]);
                foreach (var entry in new Dictionary<string, CheckBox> { { "headless", headless }, { "rawCdp", raw }, { "recordingControl", recordingControl }, { "startPaused", startPaused }, { "jsCallValues", jsCallValues }, { "detailExcludeInternal", detailExcludeInternal }, { "envExcludeInternal", envExcludeInternal }, { "closureBindings", closureBindings }, { "chunkSpool", chunkSpool }, { "streamTCP", streamTCP } }) entry.Value.Checked = options.ContainsKey(entry.Key) && Convert.ToBoolean(options[entry.Key]);
                foreach (var entry in new Dictionary<string, TextBox> { { "url", url }, { "output", output }, { "chromePath", chrome }, { "vmpProbeFile", vmpProbeFile }, { "streamHost", tcpHost }, { "streamTokenFile", tcpToken }, { "untilExpression", until }, { "envScriptInclude", envScriptInclude }, { "envScriptExclude", envScriptExclude } }) entry.Value.Text = Value(options, entry.Key);
                foreach (var entry in detailRules) entry.Value.Text = Value(options, entry.Key);
                foreach (var entry in StudioNumbers()) if (options.ContainsKey(entry.Key)) {
                    decimal value = Convert.ToDecimal(options[entry.Key]); if (new[] { "maxBytes", "maxBodyBytes", "writeBufferBytes", "vmpTypedArrayBytes" }.Contains(entry.Key)) value /= 1048576; if (entry.Key == "writeBatchBytes") value /= 1024;
                    entry.Value.Value = Math.Max(entry.Value.Minimum, Math.Min(entry.Value.Maximum, value));
                }
                envUnknownSource.SelectedIndex = Value(options, "envUnknownSource") == "exclude" ? 1 : 0; detailUnknownSource.SelectedIndex = Value(options, "detailUnknownSource") == "exclude" ? 0 : 1;
                var kinds = Value(options, "nativeKinds"); if (String.IsNullOrEmpty(kinds)) kinds = "all";
                for (int i = 0; i < nativeKinds.Items.Count; i++) nativeKinds.SetItemChecked(i, kinds.Split(',').Contains("all") || kinds.Split(',').Contains(Value(((ScenarioChoice)nativeKinds.Items[i]).Record, "id")));
                detailFiltering.Checked = detailRules.Values.Any(input => !String.IsNullOrWhiteSpace(input.Text)) || detailExcludeInternal.Checked || detailUnknownSource.SelectedIndex == 0 || kinds != "all";
                activePreset = options.ContainsKey("preset") ? Value(options, "preset") : null;
                presetStatus.Text = "已应用导入配置 · 精确预算和高级字段以配置全文 / 预检为准";
                ApplyTransportAvailability(); UpdateDetailFilters();
            } finally { changingTransport = false; applyingPreset = false; }
        }
        void RequestPreflight(Dictionary<string, object> options, bool startOnSuccess) {
            studioPreflightOptions = CloneOptions(options); startAfterPreflight = startOnSuccess;
            StudioSend("preflight", new Dictionary<string, object> { { "options", options } }, "正在校验所选能力、路径与浏览器身份（尚未启动）…");
        }
        void StudioSend(string command, Dictionary<string, object> fields, string status) {
            if (!studioAvailable || busy || studioRequestId != null) throw new InvalidOperationException("当前内核未声明工作台协议，或正在执行其他任务。");
            studioRequestKind = command; SetBusy(true); stop.Enabled = command == "viewer-index" || command.StartsWith("causality-", StringComparison.Ordinal); phase.Text = status;
            try { studioRequestId = worker.Send(command, fields); } catch { studioRequestId = null; SetBusy(false); throw; }
        }
        void StudioReady(Dictionary<string, object> message) { studioAvailable = Value(message, "studioProtocol") == "v8trace/studio/1"; sharingAvailable = Value(message, "sharingCopyProtocol") == "v8trace/sharing-copy/1"; }
        void StudioBusy() {
            foreach (var button in studioActions) button.Enabled = studioAvailable && ready && !busy;
            causalDirection.Enabled = causalKind.Enabled = causalRootId.Enabled = causalRoots.Enabled = !busy;
            causalCallChannel.Enabled = causalFunctionGrid.Enabled = !busy;
            causalFunctionBlobs.Enabled = causalIncludeAfter.Enabled = !busy;
            indexGrid.Enabled = indexedBlobs.Enabled = indexedCategory.Enabled = !busy;
            foreach (var input in indexFilters.Values) input.Enabled = !busy;
            indexNext.Enabled = studioAvailable && ready && !busy && indexHasMore && viewerGeneration != null;
            valueNext.Enabled = studioAvailable && ready && !busy && valueHasMore && viewerSelection != null; valuePrevious.Enabled = studioAvailable && ready && !busy && valueOffset > 0 && viewerSelection != null;
            foreach (Control input in new Control[] { vmpProbeFile, vmpDepth, vmpNodes, vmpProperties, vmpTypedMiB, vmpMaxActive }) input.Enabled = !busy;
            if (!studioAvailable) { configurationStatus.Text = "请同时更新 UI 与 C++ 内核，当前版本未声明工作台协议。"; start.Enabled = false; }
            ApplyEditionAvailability();
            if (shareView != null) shareView.UpdateBusy(busy);
        }

        TabPage BuildIndexedTab() {
            var tab = new TabPage("证据索引 / 全文") { Name = "IndexedEvidenceTab" }; var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5, Padding = new Padding(6) };
            panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 110)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 32)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 22)); panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 64));
            var filters = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 4, RowCount = 2 };
            for (int i = 0; i < 4; i++) filters.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 25));
            filters.RowStyles.Add(new RowStyle(SizeType.Percent,50)); filters.RowStyles.Add(new RowStyle(SizeType.Percent,50));
            var keys = new[] { "script", "function", "request", "window", "process", "isolate", "producer", "call" }; var labels = new[] { "脚本 URL / ID", "函数名称 / ID", "Request ID", "录制窗口", "Process ID", "Isolate ID", "Producer ID", "Call ID" };
            for (int i = 0; i < keys.Length; i++) { var cell = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2, Margin = new Padding(2, 0, 2, 0) }; cell.RowStyles.Add(new RowStyle(SizeType.Absolute, 20)); cell.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); var input = new TextBox { Name = "Index" + keys[i] + "Filter", Dock = DockStyle.Fill, MaxLength = 8192, AccessibleName = labels[i], Margin = new Padding(0, 2, 0, 2) }; indexFilters[keys[i]] = input; cell.Controls.Add(new Label { Text = labels[i], Dock = DockStyle.Fill, Margin = new Padding(0) }, 0, 0); cell.Controls.Add(input, 0, 1); filters.Controls.Add(cell, i % 4, i / 4); }
            panel.Controls.Add(filters, 0, 0); var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
            actions.Controls.Add(StudioButton("BuildIndexButton", "建立 / 重建", BuildIndex)); actions.Controls.Add(StudioButton("QueryIndexButton", "精确查询", () => QueryIndex(false))); actions.Controls.Add(indexNext); indexNext.Click += (sender, args) => QueryIndex(true);
            actions.Controls.Add(StudioButton("CloseIndexButton", "关闭索引", () => { StudioSend("viewer-close", new Dictionary<string, object>(), "正在清理自有索引…"); })); indexedCategory.Items.AddRange(new object[] { "", "http", "script", "env", "jscall", "native", "opcode", "wasm", "websocket", "indexeddb", "cookie", "storage", "wasm-module", "vmp", "catalog/scripts", "catalog/functions", "catalog/correlations", "gaps" }); indexedCategory.AccessibleName = "索引类别（空为全部）"; actions.Controls.Add(indexedCategory); panel.Controls.Add(actions, 0, 1);
            viewerStatus.Text = "只筛选显示，不修改生产端范围；先载入采集包，再建立临时磁盘索引。"; panel.Controls.Add(viewerStatus, 0, 2);
            var split = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Horizontal, Size = new Size(700, 460), SplitterDistance = 205, Panel1MinSize = 40, Panel2MinSize = 40 };
            bool resizingSplit = false;
            split.Resize += (sender, args) => { if (resizingSplit) return; resizingSplit = true; try { split.Orientation = split.ClientSize.Height < 180 ? Orientation.Vertical : Orientation.Horizontal; int extent = split.Orientation == Orientation.Horizontal ? split.ClientSize.Height : split.ClientSize.Width; if (extent > 84) split.SplitterDistance = Math.Max(40, Math.Min(extent - 44, extent / 2)); } finally { resizingSplit = false; } };
            indexGrid.Columns.Add("File", "类别 / 文件"); indexGrid.Columns[0].Width = 130; indexGrid.Columns.Add("Kind", "事件"); indexGrid.Columns[1].Width = 120; indexGrid.Columns.Add("Seq", "序号"); indexGrid.Columns[2].Width = 80; indexGrid.Columns.Add("Function", "函数 / 脚本"); indexGrid.Columns[3].AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill;
            indexGrid.RowTemplate.Height = 26; indexGrid.SelectionChanged += (sender, args) => { if (!renderingIndex && !busy) SelectIndexedRow(); }; split.Panel1.Controls.Add(indexGrid); split.Panel2.Controls.Add(indexedText); panel.Controls.Add(split, 0, 3);
            var values = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = true, AutoScroll = true };
            values.Controls.Add(valuePrevious); values.Controls.Add(valueNext); valuePrevious.Click += (sender, args) => ReadIndexed(Math.Max(0, valueOffset - 65536)); valueNext.Click += (sender, args) => ReadIndexed(valueNextOffset);
            values.Controls.Add(StudioButton("SaveFullEvidenceButton", "完整另存…", SaveIndexed)); values.Controls.Add(indexedBlobs); values.Controls.Add(StudioButton("ReadBlobButton", "读取引用", SelectBlob));
            values.Controls.Add(StudioButton("JumpScriptButton", "跳脚本", () => JumpIndexed("script"))); values.Controls.Add(StudioButton("JumpRequestButton", "跳请求", () => JumpIndexed("request"))); values.Controls.Add(StudioButton("JumpFunctionButton", "跳函数", () => JumpIndexed("function")));
            foreach (var input in indexFilters.Values) input.TextChanged += (sender, args) => { indexHasMore = false; StudioBusy(); };
            indexedCategory.TextChanged += (sender, args) => { indexHasMore = false; StudioBusy(); };
            panel.Controls.Add(values, 0, 4); tab.Controls.Add(panel); return tab;
        }
        void ClearViewer() { viewerGeneration = null; viewerCursor = "0"; viewerSelection = expectedLinkedRow = null; indexHasMore = valueHasMore = false; indexGrid.Rows.Clear(); indexedText.Clear(); indexedBlobs.Items.Clear(); StudioBusy(); }
        void BuildIndex() { if (captureDirectory == null) throw new InvalidOperationException("请先载入或完成一个采集包。"); ClearViewer(); StudioSend("viewer-index", new Dictionary<string, object> { { "capture", captureDirectory } }, "正在建立只读证据索引…"); }
        void QueryIndex(bool next) {
            try {
                if (viewerGeneration == null) throw new InvalidOperationException("请先建立索引。"); var options = new Dictionary<string, object> { { "after", next ? viewerCursor : "0" }, { "limit", 100 } };
                foreach (var entry in indexFilters) if (!String.IsNullOrEmpty(entry.Value.Text)) options[entry.Key] = entry.Value.Text;
                if (!String.IsNullOrEmpty(indexedCategory.Text)) options["category"] = indexedCategory.Text;
                StudioSend("viewer-query", new Dictionary<string, object> { { "options", options } }, "正在查询磁盘索引…");
            } catch (Exception error) { ShowError(error.Message); }
        }
        Dictionary<string, object> IndexedRow() { return indexGrid.SelectedRows.Count == 0 ? null : indexGrid.SelectedRows[0].Tag as Dictionary<string, object>; }
        void SelectIndexedRow() {
            var row = IndexedRow(); if (row == null) return; indexedBlobs.Items.Clear(); object refs;
            if (row.TryGetValue("blobRefs", out refs)) foreach (var item in (IEnumerable)refs) { var reference = item as Dictionary<string, object>; if (reference != null) { var label = Value(reference, "representation") + " · " + Value(reference, "bytes") + " bytes · " + Value(reference, "sha256"); var record = CloneOptions(reference); record["label"] = label; indexedBlobs.Items.Add(new ScenarioChoice { Record = record }); } }
            if (indexedBlobs.Items.Count > 0) indexedBlobs.SelectedIndex = 0;
            viewerSelection = new Dictionary<string, object> { { "rowId", Value(row, "rowId") }, { "generation", viewerGeneration } }; ReadIndexed(0);
        }
        void SelectBlob() { var choice = indexedBlobs.SelectedItem as ScenarioChoice; if (choice == null) throw new InvalidOperationException("该行没有可读取的内容引用。"); viewerSelection = new Dictionary<string, object> { { "reference", choice.Record }, { "generation", viewerGeneration } }; ReadIndexed(0); }
        void ReadIndexed(long offset) { if (viewerSelection == null) return; var options = CloneOptions(viewerSelection); options["offset"] = offset; options["limit"] = 65536; StudioSend("viewer-read", new Dictionary<string, object> { { "options", options } }, "按需读取 64 KiB 原始字节…"); }
        void SaveIndexed() {
            if (viewerSelection == null) throw new InvalidOperationException("请先选择日志行或内容引用。");
            using (var dialog = new SaveFileDialog { Filter = "完整原始字节|*.bin|原始 JSON 行|*.json|所有文件|*.*", FileName = "完整证据.bin", OverwritePrompt = false }) if (dialog.ShowDialog(this) == DialogResult.OK) { var options = CloneOptions(viewerSelection); options["output"] = dialog.FileName; StudioSend("viewer-copy", new Dictionary<string, object> { { "options", options } }, "正在原样保存完整证据（不受预览长度限制）…"); }
        }
        void JumpIndexed(string field) {
            var row = IndexedRow(); if (row == null) throw new InvalidOperationException("请先选中日志行。"); string value = field == "script" ? Value(row, "scriptUrl") : field == "function" ? Value(row, "functionId") : Value(row, "requestId"); if (String.IsNullOrEmpty(value) && field == "script") value = Value(row, "scriptId"); if (String.IsNullOrEmpty(value) && field == "function") value = Value(row, "functionName"); if (String.IsNullOrEmpty(value)) throw new InvalidOperationException("该行未直接观察到此身份；不按时间邻近猜测跳转。");
            foreach (var input in indexFilters.Values) input.Clear(); indexedCategory.Text = ""; indexFilters[field].Text = value; indexFilters["process"].Text = Value(row, "processId"); indexFilters["isolate"].Text = Value(row, "isolateId");
            if (field != "script") { indexFilters["window"].Text = Value(row, "recordingWindow"); indexFilters["producer"].Text = Value(row, "producerId"); }
            QueryIndex(false);
        }
        bool StudioMessage(Dictionary<string, object> message) {
            if (SharingMessage(message)) return true;
            var type = Value(message, "type"); if (type.StartsWith("causality-", StringComparison.Ordinal)) return CausalityMessage(message);
            if (!(type == "config-preview" || type == "preflight" || type == "studio-error" || type.StartsWith("viewer-", StringComparison.Ordinal))) return false;
            if (Value(message, "id") != studioRequestId) return true;
            if (type == "viewer-progress") { viewerStatus.Text = "已索引 " + Value(message, "records") + " 行 · " + Value(message, "source"); return true; }
            var kind = studioRequestKind; studioRequestId = null; studioRequestKind = null; SetBusy(false);
            if (type == "studio-error" && kind != null && kind.StartsWith("share-", StringComparison.Ordinal) && shareView != null) {
                shareView.Review = null; shareView.Preview.Text = Pretty(message); shareView.Status.Text = "任务未完成；原始目录保留，新目录若已创建则保留部分文件 / 失败回执：" + Value(message, "error");
                shareView.UpdateBusy(false); if (shareView.CloseWhenIdle) shareView.Window.Close(); return true;
            }
            if (type == "studio-error") { startAfterPreflight = false; if (guidedPreflightPending) { guidedPreflightPending = false; guidedStatus.Text = "预检任务错误；保留参数，核对摘要后手动修正。"; } ShowError(Value(message, "error")); return true; }
            if (type == "config-preview") {
                pendingConfiguration = ObjectValue(message, "document"); configurationText.Text = Pretty(pendingConfiguration); var report = CloneOptions(message); report.Remove("document"); summaryText.Text = Pretty(report);
                var structure = ObjectValue(message, "structuralPreflight");
                if (structure == null || Value(structure, "ok") != "True") { configurationStatus.Text = "配置未应用：" + Value(structure, "error"); return true; }
                using (var dialog = new Form { Text = "配置差异预览 · 确认后应用", StartPosition = FormStartPosition.CenterParent, Size = new Size(850, 620), MinimizeBox = false, MaximizeBox = false }) {
                    var text = new RichTextBox { Dock = DockStyle.Fill, ReadOnly = true, WordWrap = false, Font = new Font("Consolas", 10F), Text = Pretty(report) };
                    var buttons = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 42, FlowDirection = FlowDirection.RightToLeft }; var cancel = new Button { Text = "取消", DialogResult = DialogResult.Cancel }; var apply = new Button { Text = "确认应用", DialogResult = DialogResult.OK, Width = 110 }; buttons.Controls.Add(cancel); buttons.Controls.Add(apply); dialog.Controls.Add(text); dialog.Controls.Add(buttons); dialog.AcceptButton = apply; dialog.CancelButton = cancel;
                    if (dialog.ShowDialog(this) == DialogResult.OK) { ApplyConfigurationDocument(ObjectValue(pendingConfiguration, "options"), ObjectValue(structure, "plan")); configurationStatus.Text = "已应用；未知字段/精确预算完整保留。启动前仍核对真实浏览器。"; } else configurationStatus.Text = "已取消应用；当前配置未变，编辑稿保留供核对。";
                }
            } else if (type == "preflight") {
                if (guidedPreflightPending) { guidedPreflightPending = false; guidedStatus.Text = Value(message, "ok") == "True" ?
                    "上次预检通过：下一步手动开始。调整参数后，开始时会重新校验。" : "预检失败：查看右侧实际原因并修正；没有启动或自动切换浏览器。"; }
                summaryText.Text = Pretty(message); configurationStatus.Text = Value(message, "ok") == "True" ? "预检通过（非网页运行验收）；报告含实际路径、范围与预算。" : "预检失败：" + Value(message, "error"); phase.Text = configurationStatus.Text;
                if (startAfterPreflight) { startAfterPreflight = false; if (Value(message, "ok") == "True") { object unknown; var fields = message.TryGetValue("unknownFields", out unknown) ? ((IEnumerable)unknown).Cast<object>().Count() : 0; if (fields == 0 || MessageBox.Show(this, "配置有 " + fields + " 个未知字段，当前内核不执行。报告已列明；是否按已知选项开始？", "核对配置范围", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) == DialogResult.OK) StartCaptureNow(studioPreflightOptions); } else ShowError(Value(message, "error")); }
            } else if (type == "viewer-index") { viewerGeneration = Value(message, "generation"); viewerStatus.Text = "已索引 " + Value(message, "records") + " 行；显示边界 " + Value(message, "indexBoundaryCount") + "。不代表完整因果。"; QueryIndex(false); }
            else if (type == "viewer-closed") { ClearViewer(); viewerStatus.Text = "索引已关闭，自有临时目录已清理；原日志保持不变。"; }
            else if (type == "viewer-query") {
                if (Value(message, "generation") != viewerGeneration) return true; renderingIndex = true; indexGrid.Rows.Clear(); object rows;
                if (message.TryGetValue("rows", out rows)) foreach (var item in (IEnumerable)rows) { var row = item as Dictionary<string, object>; if (row == null) continue; var at = indexGrid.Rows.Add(Value(row, "category"), Value(row, "kind"), Value(row, "seq"), Value(row, "functionName") + " " + Value(row, "scriptUrl")); indexGrid.Rows[at].Tag = row; }
                renderingIndex = false; viewerCursor = Value(message, "nextCursor"); indexHasMore = Value(message, "hasMore") == "True"; StudioBusy(); viewerStatus.Text = "本页 " + indexGrid.Rows.Count + " 行 / 总源行 " + Value(message, "records") + "；元数据精确匹配，非生产端过滤。";
                var expected = expectedLinkedRow; expectedLinkedRow = null; DataGridViewRow selected = null;
                foreach (DataGridViewRow candidate in indexGrid.Rows) {
                    var row = candidate.Tag as Dictionary<string, object>;
                    if (expected == null || (Value(row, "source") == Value(expected, "source") && Value(row, "line") == Value(expected, "line") && Value(row, "sourceFileSha256") == Value(expected, "sourceFileSha256"))) { selected = candidate; break; }
                }
                renderingIndex = true; indexGrid.ClearSelection(); if (selected != null) selected.Selected = true; renderingIndex = false;
                if (selected != null) SelectIndexedRow(); else { viewerSelection = null; indexedText.Clear(); indexedBlobs.Items.Clear(); valueHasMore = false; StudioBusy(); }
                if (expected != null) viewerStatus.Text = selected == null ? "证据不足：本页未找到相同文件 SHA-256 / 行身份；不自动选择相近记录。" : "已确认同一原始文件 SHA-256 / 行；request/process/isolate/window 等身份按实际观察值传递。";
            } else if (type == "viewer-read") {
                valueOffset = Count(message, "offset"); valueNextOffset = Count(message, "nextOffset"); valueHasMore = Value(message, "hasMore") == "True";
                indexedText.Text = "[原始字节 " + valueOffset + " .. " + valueNextOffset + " / " + Value(message, "totalBytes") + "；UTF-8 显示可能替换边界码元，完整另存保留原始字节]\n\n" + Encoding.UTF8.GetString(Convert.FromBase64String(Value(message, "base64"))); StudioBusy();
                phase.Text = "已读取字节页 · 原始完整值保持不变";
            } else if (type == "viewer-copy") { viewerStatus.Text = "完整另存 " + Value(message, "bytes") + " bytes · SHA-256 " + Value(message, "sha256"); phase.Text = "已保存完整原始证据"; }
            return true;
        }
    }
}
