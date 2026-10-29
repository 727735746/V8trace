using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.Windows.Forms;

namespace V8Trace {
    public sealed partial class TraceWindow {
        readonly Label causalStatus = new Label { Name = "CausalStatus", Dock = DockStyle.Fill, AutoEllipsis = true, Text = "选择已完成的采集包，再建立只读因果图。未闭合、缺失桥与录制边界将原样显示。" };
        readonly TextBox causalRootId = new TextBox { Name = "CausalRootId", Width = 280, AccessibleName = "HTTP 根 ID" };
        readonly ComboBox causalDirection = new ComboBox { Name = "CausalDirection", Width = 180, DropDownStyle = ComboBoxStyle.DropDownList, AccessibleName = "请求因果方向" };
        readonly ComboBox causalKind = new ComboBox { Name = "CausalKind", Width = 130, DropDownStyle = ComboBoxStyle.DropDownList, AccessibleName = "关键证据点类别" };
        readonly ComboBox causalCallChannel = new ComboBox { Name = "CausalCallChannel", Width = 110, DropDownStyle = ComboBoxStyle.DropDownList, AccessibleName = "逐函数插桩通道" };
        readonly CheckBox causalIncludeAfter = new CheckBox { Name = "CausalIncludeAfter", AutoSize = true, Text = "包含发送后记录 / 返回", AccessibleName = "包含请求发送后的调用返回记录" };
        readonly ComboBox causalFunctionBlobs = new ComboBox { Name = "CausalFunctionBlobs", Width = 170, DropDownStyle = ComboBoxStyle.DropDownList, AccessibleName = "函数值 / HTTP payload 原始内容引用" };
        readonly DataGridView causalFunctionGrid = new DataGridView { Name = "CausalFunctionGrid", Dock = DockStyle.Fill, ReadOnly = true, AllowUserToAddRows = false, AllowUserToDeleteRows = false, RowHeadersVisible = false, SelectionMode = DataGridViewSelectionMode.FullRowSelect, MultiSelect = false, BackgroundColor = Color.White };
        readonly DataGridView causalRoots = new DataGridView { Name = "CausalRoots", Dock = DockStyle.Fill, ReadOnly = true, AllowUserToAddRows = false, AllowUserToDeleteRows = false, RowHeadersVisible = false, SelectionMode = DataGridViewSelectionMode.FullRowSelect, MultiSelect = false, BackgroundColor = Color.White };
        readonly RichTextBox causalDetails = new RichTextBox { Name = "CausalDetails", Dock = DockStyle.Fill, ReadOnly = true, WordWrap = false, Font = new Font("Consolas", 9F) };
        string causalGeneration, causalRootCursor, causalNodeCursor, causalCheckpointCursor, causalFunctionCursor;
        SplitContainer causalFunctionSplit;
        Dictionary<string, object> causalFunctionSelection;
        long causalValueOffset, causalValueNext;
        bool renderingCausalFunctions, causalValueMore;
        TabPage BuildCausalityTab() {
            var tab = new TabPage("HTTP 根链 / 插桩") { Name = "CausalityTab" };
            var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5, Padding = new Padding(6) };
            panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 38)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 28)); panel.RowStyles.Add(new RowStyle(SizeType.Percent, 40)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 110)); panel.RowStyles.Add(new RowStyle(SizeType.Percent, 60));
            var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = true, AutoScroll = true };
            actions.Controls.Add(StudioButton("CausalBuild", "建立 / 重建因果图", () => {
                if (captureDirectory == null) throw new InvalidOperationException("请先载入或完成采集包。");
                causalGeneration = causalRootCursor = causalNodeCursor = causalCheckpointCursor = null; causalRoots.Rows.Clear(); causalDetails.Clear(); causalRootId.Clear();
                StudioSend("causality-index", new Dictionary<string, object> { { "capture", captureDirectory } }, "正在索引原始因果账本…");
            }));
            actions.Controls.Add(StudioButton("CausalRootsButton", "HTTP 根列表", () => CausalQuery("roots", false)));
            actions.Controls.Add(StudioButton("CausalRootsNext", "下一页根", () => { if (String.IsNullOrEmpty(causalRootCursor)) throw new InvalidOperationException("没有下一页根。"); CausalQuery("roots", true); }));
            actions.Controls.Add(StudioButton("CausalClose", "关闭图", () => StudioSend("causality-close", new Dictionary<string, object>(), "关闭自有图索引…")));
            panel.Controls.Add(actions, 0, 0); panel.Controls.Add(causalStatus, 0, 1);
            causalRoots.Columns.Add("Root", "HTTP 根 ID"); causalRoots.Columns[0].Width = 200; causalRoots.Columns.Add("State", "实际状态"); causalRoots.Columns[1].Width = 130; causalRoots.Columns.Add("Url", "请求 / 证据"); causalRoots.Columns[2].AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill;
            causalRoots.SelectionChanged += (sender, args) => { if (causalRoots.SelectedRows.Count == 0) return; var row = causalRoots.SelectedRows[0].Tag as Dictionary<string, object>; if (row != null) { causalRootId.Text = Value(row, "rootId"); causalNodeCursor = causalCheckpointCursor = null; causalDetails.Text = Pretty(row); } };
            panel.Controls.Add(causalRoots, 0, 2);
            var query = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = true, AutoScroll = true };
            query.Controls.Add(new Label { Text = "HTTP 根 ID", AutoSize = true, Padding = new Padding(0, 7, 0, 0) }); query.Controls.Add(causalRootId);
            causalDirection.Items.AddRange(new object[] { "触发入口 → HTTP 发送", "HTTP → 后代结束" }); causalDirection.SelectedIndex = 0; query.Controls.Add(causalDirection);
            causalKind.Items.AddRange(new object[] { "全部关键点", "加密参数 / 结果", "DOM 解析 / 依赖", "HTTP 发送" }); causalKind.SelectedIndex = 0; query.Controls.Add(causalKind);
            causalCallChannel.Items.AddRange(new object[] { "jscall", "native", "opcode", "wasm", "http" }); causalCallChannel.SelectedIndex = 0; query.Controls.Add(causalCallChannel);
            causalCallChannel.SelectedIndexChanged += (sender, args) => ClearCausalFunctions();
            causalRootId.TextChanged += (sender, args) => ClearCausalFunctions();
            causalIncludeAfter.CheckedChanged += (sender, args) => ClearCausalFunctions(); query.Controls.Add(causalIncludeAfter);
            causalDirection.SelectedIndexChanged += (sender, args) => { causalNodeCursor = causalCheckpointCursor = null; ClearCausalFunctions(); }; causalKind.SelectedIndexChanged += (sender, args) => { causalCheckpointCursor = null; };
            query.Controls.Add(StudioButton("CausalRoot", "根详情 / 闭合证明", () => CausalQuery("root", false)));
            query.Controls.Add(StudioButton("CausalNodes", "遍历节点", () => CausalQuery("nodes", false)));
            query.Controls.Add(StudioButton("CausalNodesNext", "下一页节点", () => { if (String.IsNullOrEmpty(causalNodeCursor)) throw new InvalidOperationException("没有下一页节点。"); CausalQuery("nodes", true); }));
            query.Controls.Add(StudioButton("CausalCheckpoints", "参数 / payload / DOM 关键点", () => CausalQuery("checkpoints", false)));
            query.Controls.Add(StudioButton("CausalCheckpointsNext", "下一页关键点", () => { if (String.IsNullOrEmpty(causalCheckpointCursor)) throw new InvalidOperationException("没有下一页关键点。"); CausalQuery("checkpoints", true); }));
            query.Controls.Add(StudioButton("CausalFunctions", "逐函数 / 方法插桩", () => CausalQuery("functions", false)));
            query.Controls.Add(StudioButton("CausalFunctionsNext", "下一页调用", () => { if (String.IsNullOrEmpty(causalFunctionCursor)) throw new InvalidOperationException("没有下一页调用。"); CausalQuery("functions", true); }));
            query.Controls.Add(causalFunctionBlobs);
            query.Controls.Add(StudioButton("CausalReadPayload", "读取完整 payload / 引用", () => { var choice = causalFunctionBlobs.SelectedItem as ScenarioChoice; if (choice == null || causalFunctionSelection == null) throw new InvalidOperationException("请先选择带正文引用的HTTP / 函数记录。"); causalFunctionSelection = new Dictionary<string, object> { { "reference", choice.Record }, { "generation", causalFunctionSelection["generation"] } }; ReadCausalFunction(0); }));
            query.Controls.Add(StudioButton("CausalArgumentsNext", "下一段原始参数", () => { if (!causalValueMore) throw new InvalidOperationException("没有下一段原始字节。"); ReadCausalFunction(causalValueNext); }));
            query.Controls.Add(StudioButton("CausalArgumentsPrevious", "前一段原始参数", () => ReadCausalFunction(Math.Max(0, causalValueOffset - 65536))));
            panel.Controls.Add(query, 0, 3);
            causalFunctionSplit = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Horizontal, Size = new Size(700, 360), SplitterDistance = 120, Panel1MinSize = 40, Panel2MinSize = 40, Panel1Collapsed = true };
            bool resizing = false; causalFunctionSplit.Resize += (sender, args) => { if (resizing) return; resizing = true; try { causalFunctionSplit.Orientation = causalFunctionSplit.ClientSize.Height < 180 ? Orientation.Vertical : Orientation.Horizontal; int extent = causalFunctionSplit.Orientation == Orientation.Horizontal ? causalFunctionSplit.ClientSize.Height : causalFunctionSplit.ClientSize.Width; if (extent > 84) causalFunctionSplit.SplitterDistance = Math.Max(40, Math.Min(extent - 44, extent / 3)); } finally { resizing = false; } };
            causalFunctionGrid.Columns.Add("Function", "函数 / 方法"); causalFunctionGrid.Columns[0].Width = 150; causalFunctionGrid.Columns.Add("Phase", "阶段"); causalFunctionGrid.Columns[1].Width = 65; causalFunctionGrid.Columns.Add("Call", "调用 ID"); causalFunctionGrid.Columns[2].Width = 85; causalFunctionGrid.Columns.Add("Source", "源码 / 请求位置"); causalFunctionGrid.Columns[3].AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill;
            causalFunctionGrid.SelectionChanged += (sender, args) => { if (renderingCausalFunctions || busy || causalFunctionGrid.SelectedRows.Count == 0) return; var row = causalFunctionGrid.SelectedRows[0].Tag as Dictionary<string, object>; if (row == null) return; causalFunctionSelection = new Dictionary<string, object> { { "rowId", Value(row, "rowId") }, { "generation", Value(row, "generation") } }; causalFunctionBlobs.Items.Clear(); object refs; int upload = -1;
                if (row.TryGetValue("blobRefs", out refs)) foreach (var item in (IEnumerable)refs) { var reference = item as Dictionary<string, object>; if (reference == null) continue; var entry = CloneOptions(reference); entry["label"] = Value(reference, "representation") + " · " + Value(reference, "bytes") + " bytes"; int at = causalFunctionBlobs.Items.Add(new ScenarioChoice { Record = entry }); if (Value(reference, "representation").Contains("upload")) upload = at; }
                if (causalFunctionBlobs.Items.Count > 0) causalFunctionBlobs.SelectedIndex = upload >= 0 ? upload : 0; ReadCausalFunction(0); };
            causalFunctionSplit.Panel1.Controls.Add(causalFunctionGrid); causalFunctionSplit.Panel2.Controls.Add(causalDetails); panel.Controls.Add(causalFunctionSplit, 0, 4); tab.Controls.Add(panel); return tab;
        }
        void ClearCausalFunctions() { causalFunctionCursor = null; causalFunctionSelection = null; causalValueMore = false; causalFunctionGrid.Rows.Clear(); causalFunctionBlobs.Items.Clear(); }
        void ReadCausalFunction(long offset) { if (causalFunctionSelection == null) throw new InvalidOperationException("请先选择一条函数调用证据。"); var options = CloneOptions(causalFunctionSelection); options["offset"] = offset; options["limit"] = 65536; StudioSend("causality-function-read", new Dictionary<string, object> { { "options", options } }, "按需读取完整函数实参 / 返回 / 异常的原始字节…"); }
        void CausalQuery(string kind, bool next) {
            if (String.IsNullOrEmpty(causalGeneration)) throw new InvalidOperationException("请先建立因果图。");
            var options = new Dictionary<string, object> { { "generation", causalGeneration }, { "limit", 100 } };
            if (kind != "roots") { if (String.IsNullOrWhiteSpace(causalRootId.Text)) throw new InvalidOperationException("请从列表选择一个 HTTP 根。"); options["rootId"] = causalRootId.Text.Trim(); }
            if (kind == "nodes" || kind == "checkpoints" || kind == "functions") options["direction"] = causalDirection.SelectedIndex == 0 ? "upstream" : "downstream";
            if (kind == "checkpoints") options["kindPrefix"] = new[] { "", "crypto-", "dom-", "http-" }[causalKind.SelectedIndex];
            if (kind == "functions") { options["channel"] = causalCallChannel.Text; options["includeAfterRequest"] = causalIncludeAfter.Checked; }
            if (next) options["cursor"] = kind == "roots" ? causalRootCursor : kind == "checkpoints" ? causalCheckpointCursor : kind == "functions" ? causalFunctionCursor : causalNodeCursor;
            StudioSend("causality-" + kind, new Dictionary<string, object> { { "options", options } }, "正在读取原始因果图证据…");
        }
        bool CausalityMessage(Dictionary<string, object> message) {
            if (Value(message, "id") != studioRequestId) return true;
            var type = Value(message, "type");
            if (type == "causality-progress") { causalStatus.Text = "正在建立因果图：" + Value(message, "records") + " · " + Value(message, "source"); return true; }
            studioRequestId = studioRequestKind = null; SetBusy(false);
            if (type == "causality-function-read") { causalValueOffset = Count(message, "offset"); causalValueNext = Count(message, "nextOffset"); causalValueMore = Value(message, "hasMore") == "True"; causalDetails.Text = "[原始字节 " + causalValueOffset + " .. " + causalValueNext + " / " + Value(message, "totalBytes") + "；本页 UTF-8 显示，原始内容保持完整]\n\n" + System.Text.Encoding.UTF8.GetString(Convert.FromBase64String(Value(message, "base64"))); return true; }
            causalFunctionSplit.Panel1Collapsed = type != "causality-functions";
            if (type == "causality-closed") { causalGeneration = null; causalRootCursor = causalNodeCursor = causalCheckpointCursor = null; causalRoots.Rows.Clear(); causalDetails.Clear(); causalRootId.Clear(); causalStatus.Text = "因果图已关闭；原始日志保持不变。"; return true; }
            causalDetails.Text = Pretty(message);
            var report = ObjectValue(message, "root") ?? message;
            causalStatus.Text = "状态：" + Value(report, "status") + " · 因果完整：" + Value(report, "completeCausality") + " · 以各根证明和 reasonCodes 为准，不等于算法已还原。";
            if (type == "causality-index") { causalGeneration = Value(message, "generation"); if (!String.IsNullOrEmpty(causalGeneration)) CausalQuery("roots", false); }
            else if (type == "causality-roots") {
                causalRoots.Rows.Clear(); causalRootCursor = Value(message, "hasMore") == "True" ? Value(message, "nextCursor") : null; object rows;
                if (message.TryGetValue("rows", out rows)) foreach (var item in (IEnumerable)rows) { var row = item as Dictionary<string, object>; if (row == null) continue; var at = causalRoots.Rows.Add(Value(row, "rootId"), Value(row, "status"), Value(ObjectValue(row, "request"), "url")); causalRoots.Rows[at].Tag = row; }
                if (causalRoots.Rows.Count > 0) { causalRoots.ClearSelection(); causalRoots.Rows[0].Selected = true; }
            } else if (type == "causality-nodes") causalNodeCursor = Value(message, "hasMore") == "True" ? Value(message, "nextCursor") : null;
            else if (type == "causality-checkpoints") causalCheckpointCursor = Value(message, "hasMore") == "True" ? Value(message, "nextCursor") : null;
            else if (type == "causality-functions") {
                causalFunctionCursor = Value(message, "hasMore") == "True" ? Value(message, "nextCursor") : null; causalFunctionSelection = null; causalValueMore = false; renderingCausalFunctions = true; causalFunctionGrid.Rows.Clear(); object rows;
                if (message.TryGetValue("rows", out rows)) foreach (var item in (IEnumerable)rows) { var row = item as Dictionary<string, object>; if (row == null) continue; var name = Value(row, "functionName"); var at = causalFunctionGrid.Rows.Add(String.IsNullOrEmpty(name) ? Value(row, "kind") : name, Value(row, "phase"), Value(row, "callId"), Value(row, "scriptUrl") + " · " + Value(row, "requestPosition")); causalFunctionGrid.Rows[at].Tag = row; }
                renderingCausalFunctions = false; causalStatus.Text = "逐次调用 " + causalFunctionGrid.Rows.Count + " 行；按因果节点及原始行排列，非全局时间顺序。递归分别保留；选择一行读取实参、返回与异常。";
                if (causalFunctionGrid.Rows.Count > 0) { causalFunctionGrid.ClearSelection(); causalFunctionGrid.Rows[0].Selected = true; }
            }
            return true;
        }
    }
}
