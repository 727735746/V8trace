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
        void StudioReady(Dictionary<string, object> message) { studioAvailable = Value(message, "studioProtocol") == "v8trace/studio/1"; }
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
        }

        TabPage BuildIndexedTab() {
            var tab = new TabPage("证据索引 / 全文") { Name = "IndexedEvidenceTab" }; var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5, Padding = new Padding(6) };
            panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 90)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 32)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 22)); panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 64));
            var filters = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 4, RowCount = 2 };
            for (int i = 0; i < 4; i++) filters.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 25));
            filters.RowStyles.Add(new RowStyle(SizeType.Percent,50)); filters.RowStyles.Add(new RowStyle(SizeType.Percent,50));
            var keys = new[] { "script", "function", "request", "window", "process", "isolate", "producer", "call" }; var labels = new[] { "脚本 URL / ID", "函数名称 / ID", "Request ID", "录制窗口", "Process ID", "Isolate ID", "Producer ID", "Call ID" };
            for (int i = 0; i < keys.Length; i++) { var cell = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2, Margin = new Padding(2, 0, 2, 0) }; cell.RowStyles.Add(new RowStyle(SizeType.Absolute, 18)); cell.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); var input = new TextBox { Name = "Index" + keys[i] + "Filter", Dock = DockStyle.Fill, MaxLength = 8192, AccessibleName = labels[i] }; indexFilters[keys[i]] = input; cell.Controls.Add(new Label { Text = labels[i], Dock = DockStyle.Fill }, 0, 0); cell.Controls.Add(input, 0, 1); filters.Controls.Add(cell, i % 4, i / 4); }
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
        void ClearViewer() { viewerGeneration = null; viewerCursor = "0"; viewerSelection = null; indexHasMore = valueHasMore = false; indexGrid.Rows.Clear(); indexedText.Clear(); indexedBlobs.Items.Clear(); StudioBusy(); }
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
            foreach (var input in indexFilters.Values) input.Clear(); indexedCategory.Text = ""; indexFilters[field].Text = value; indexFilters["process"].Text = Value(row, "processId"); indexFilters["isolate"].Text = Value(row, "isolateId"); QueryIndex(false);
        }
        bool StudioMessage(Dictionary<string, object> message) {
            var type = Value(message, "type"); if (type.StartsWith("causality-", StringComparison.Ordinal)) return CausalityMessage(message);
            if (!(type == "config-preview" || type == "preflight" || type == "studio-error" || type.StartsWith("viewer-", StringComparison.Ordinal))) return false;
            if (Value(message, "id") != studioRequestId) return true;
            if (type == "viewer-progress") { viewerStatus.Text = "已索引 " + Value(message, "records") + " 行 · " + Value(message, "source"); return true; }
            var kind = studioRequestKind; studioRequestId = null; studioRequestKind = null; SetBusy(false);
            if (type == "studio-error") { startAfterPreflight = false; ShowError(Value(message, "error")); return true; }
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
                summaryText.Text = Pretty(message); configurationStatus.Text = Value(message, "ok") == "True" ? "预检通过（非网页运行验收）；报告含实际路径、范围与预算。" : "预检失败：" + Value(message, "error"); phase.Text = configurationStatus.Text;
                if (startAfterPreflight) { startAfterPreflight = false; if (Value(message, "ok") == "True") { object unknown; var fields = message.TryGetValue("unknownFields", out unknown) ? ((IEnumerable)unknown).Cast<object>().Count() : 0; if (fields == 0 || MessageBox.Show(this, "配置有 " + fields + " 个未知字段，当前内核不执行。报告已列明；是否按已知选项开始？", "核对配置范围", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) == DialogResult.OK) StartCaptureNow(studioPreflightOptions); } else ShowError(Value(message, "error")); }
            } else if (type == "viewer-index") { viewerGeneration = Value(message, "generation"); viewerStatus.Text = "已索引 " + Value(message, "records") + " 行；显示边界 " + Value(message, "indexBoundaryCount") + "。不代表完整因果。"; QueryIndex(false); }
            else if (type == "viewer-closed") { ClearViewer(); viewerStatus.Text = "索引已关闭，自有临时目录已清理；原日志保持不变。"; }
            else if (type == "viewer-query") {
                if (Value(message, "generation") != viewerGeneration) return true; renderingIndex = true; indexGrid.Rows.Clear(); object rows;
                if (message.TryGetValue("rows", out rows)) foreach (var item in (IEnumerable)rows) { var row = item as Dictionary<string, object>; if (row == null) continue; var at = indexGrid.Rows.Add(Value(row, "category"), Value(row, "kind"), Value(row, "seq"), Value(row, "functionName") + " " + Value(row, "scriptUrl")); indexGrid.Rows[at].Tag = row; }
                renderingIndex = false; viewerCursor = Value(message, "nextCursor"); indexHasMore = Value(message, "hasMore") == "True"; StudioBusy(); viewerStatus.Text = "本页 " + indexGrid.Rows.Count + " 行 / 总源行 " + Value(message, "records") + "；元数据精确匹配，非生产端过滤。";
                if (indexGrid.Rows.Count > 0) { renderingIndex = true; indexGrid.ClearSelection(); indexGrid.Rows[0].Selected = true; renderingIndex = false; SelectIndexedRow(); } else { viewerSelection = null; indexedText.Clear(); indexedBlobs.Items.Clear(); valueHasMore = false; StudioBusy(); }
            } else if (type == "viewer-read") {
                valueOffset = Count(message, "offset"); valueNextOffset = Count(message, "nextOffset"); valueHasMore = Value(message, "hasMore") == "True";
                indexedText.Text = "[原始字节 " + valueOffset + " .. " + valueNextOffset + " / " + Value(message, "totalBytes") + "；UTF-8 显示可能替换边界码元，完整另存保留原始字节]\n\n" + Encoding.UTF8.GetString(Convert.FromBase64String(Value(message, "base64"))); StudioBusy();
                phase.Text = "已读取字节页 · 原始完整值保持不变";
            } else if (type == "viewer-copy") { viewerStatus.Text = "完整另存 " + Value(message, "bytes") + " bytes · SHA-256 " + Value(message, "sha256"); phase.Text = "已保存完整原始证据"; }
            return true;
        }
    }
}
