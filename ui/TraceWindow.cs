using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace V8Trace {
    public sealed partial class TraceWindow : Form {
        readonly Color ink = Color.FromArgb(25, 42, 61);
        readonly Color accent = Color.FromArgb(0, 117, 120);
        readonly WorkerClient worker = new WorkerClient();
        readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = 8 * 1024 * 1024 };
        readonly Dictionary<string, CheckBox> channels = new Dictionary<string, CheckBox>();
        readonly List<Dictionary<string, object>> previews = new List<Dictionary<string, object>>();
        readonly Timer timer = new Timer { Interval = 200 };
        readonly TextBox url = new TextBox { Name = "UrlInput", Text = "https://example.test/" };
        readonly TextBox chrome = new TextBox { Name = "ChromeInput" };
        readonly TextBox output = new TextBox { Name = "OutputInput" };
        readonly TextBox until = new TextBox { Name = "UntilInput" };
        readonly TextBox exportOutput = new TextBox { Name = "ExportOutputInput" };
        readonly NumericUpDown duration = Number("DurationInput", 60, 1, 86400);
        readonly NumericUpDown maxEvents = Number("EventsBudgetInput", 100000, 1, 5000000);
        readonly NumericUpDown maxMiB = Number("BytesBudgetInput", 256, 1, 4096);
        readonly NumericUpDown bodyMiB = Number("BodyBudgetInput", 16, 1, 1024);
        readonly NumericUpDown writeBufferMiB = Number("WriteBufferInput", 8, 1, 256);
        readonly NumericUpDown writeBatchKiB = Number("WriteBatchInput", 1024, 4, 4096);
        readonly NumericUpDown objectDepth = Number("ObjectDepthInput", 2, 1, 8);
        readonly NumericUpDown objectNodes = Number("ObjectNodesInput", 64, 1, 4096);
        readonly NumericUpDown objectProperties = Number("ObjectPropertiesInput", 256, 1, 65536);
        readonly CheckBox headless = new CheckBox { Name = "HeadlessOption", Text = "无头模式（自动化测试）", AutoSize = true };
        readonly CheckBox raw = new CheckBox { Name = "RawOption", Text = "保留已启用通道的原始 CDP 消息", AutoSize = true };
        readonly Button start = new Button { Name = "StartButton", Text = "开始采集", Height = 40 };
        readonly Button stop = new Button { Name = "StopButton", Text = "停止并保存", Height = 40, Enabled = false };
        readonly Button pauseRecording = new Button { Name = "PauseRecordingButton", Text = "暂停采集", Height = 40, Enabled = false };
        readonly CheckBox recordingControl = new CheckBox { Name = "RecordingControlOption", Text = "启用随停随开（浏览器保持运行）", Checked = true, AutoSize = true };
        readonly CheckBox startPaused = new CheckBox { Name = "StartPausedOption", Text = "启动后先不采集，手动开启", AutoSize = true };
        readonly Button export = new Button { Name = "ExportButton", Text = "导出案例", Width = 92, Enabled = false };
        readonly Button open = new Button { Name = "OpenOutputButton", Text = "打开日志目录", Width = 122, Enabled = false };
        readonly Button load = new Button { Name = "LoadCaptureButton", Text = "载入采集包", Width = 105 };
        readonly Button refresh = new Button { Name = "RefreshEventsButton", Text = "读取日志尾部", Width = 124, Enabled = false };
        readonly TextBox tcpHost = new TextBox { Name = "TcpHostInput", Text = "127.0.0.1" };
        readonly NumericUpDown tcpPort = Number("TcpPortInput", 17880, 1, 65535);
        readonly TextBox tcpToken = new TextBox { Name = "TcpTokenFileInput" };
        readonly CheckBox tcpAfterCapture = new CheckBox { Name = "TcpAfterCaptureOption", Text = "采集保存后自动 TCP 传输（保留本地原始日志）", AutoSize = true };
        readonly CheckBox chunkSpool = new CheckBox { Name = "ChunkSpoolOption", Text = "持久无损分块归档（完整原始目录同时保留）", AutoSize = true };
        readonly CheckBox streamTCP = new CheckBox { Name = "StreamTcpOption", Text = "宿主日志产生后持续 TCP 搬运（独立最终提交）", AutoSize = true };
        readonly Button tcpSend = new Button { Name = "TcpSendButton", Text = "传输当前采集包", Enabled = false, Height = 36 };
        readonly Label tcpStatus = new Label { Name = "TcpStatusLabel", Text = "等待 C++ TCP 内核就绪", AutoSize = true };
        bool tcpAvailable;
        readonly TextBox vmpProbeFile = new TextBox { Name = "VmpProbeFileInput" };
        readonly NumericUpDown vmpDepth = Number("VmpDepthInput", 2, 1, 8), vmpNodes = Number("VmpNodesInput", 256, 1, 4096), vmpProperties = Number("VmpPropertiesInput", 4096, 1, 65536), vmpTypedMiB = Number("VmpTypedMiBInput", 16, 1, 64), vmpMaxActive = Number("VmpMaxActiveInput", 4096, 1, 65536);
        readonly ComboBox category = new ComboBox { Name = "CategoryFilter", DropDownStyle = ComboBoxStyle.DropDownList, Width = 125 };
        readonly TextBox filter = new TextBox { Name = "TextFilter", Width = 175 };
        readonly CheckBox detailFiltering = new CheckBox { Name = "DetailFilteringOption", Text = "启用 JSCall / Native 生产端过滤", AutoSize = true };
        readonly CheckBox jsCallValues = new CheckBox { Name = "JsCallValuesOption", Text = "JSCall 参数 / this / 返回值 / 异常详情（默认完整采集）", Checked = true, AutoSize = true };
        readonly CheckBox detailExcludeInternal = new CheckBox { Name = "DetailExcludeInternalOption", Text = "排除采集器内部、浏览器 UI 和扩展脚本", Checked = true, AutoSize = true };
        readonly ComboBox detailUnknownSource = new ComboBox { Name = "DetailUnknownSourceInput", DropDownStyle = ComboBoxStyle.DropDownList };
        readonly CheckedListBox nativeKinds = new CheckedListBox { Name = "NativeKindsInput", CheckOnClick = true, IntegralHeight = false };
        readonly Dictionary<string, TextBox> detailRules = new Dictionary<string, TextBox>();
        readonly TextBox envScriptInclude = new TextBox { Name = "EnvScriptIncludeInput", Multiline = true, ScrollBars = ScrollBars.Vertical, MaxLength = 8192 };
        readonly TextBox envScriptExclude = new TextBox { Name = "EnvScriptExcludeInput", Multiline = true, ScrollBars = ScrollBars.Vertical, MaxLength = 8192 };
        readonly CheckBox envExcludeInternal = new CheckBox { Name = "EnvExcludeInternalOption", Text = "排除采集器与浏览器内部来源", AutoSize = true };
        readonly ComboBox envUnknownSource = new ComboBox { Name = "EnvUnknownSourceInput", DropDownStyle = ComboBoxStyle.DropDownList };
        readonly NumericUpDown envInventoryProperties = Number("EnvInventoryPropertiesInput", 65536, 1, 1048576);
        readonly CheckBox closureBindings = new CheckBox { Name = "ClosureBindingsOption", Text = "定向闭包绑定快照（跟随 JSCall 具体白名单）", AutoSize = true };
        readonly NumericUpDown closureScopes = Number("ClosureScopesInput", 32, 1, 128);
        readonly NumericUpDown closureBindingsLimit = Number("ClosureBindingsLimitInput", 4096, 1, 1048576);
        readonly NumericUpDown closureDepth = Number("ClosureDepthInput", 2, 1, 8);
        readonly NumericUpDown closureNodes = Number("ClosureNodesInput", 256, 1, 4096);
        readonly NumericUpDown closureProperties = Number("ClosurePropertiesInput", 4096, 1, 65536);
        readonly Label phase = new Label { Name = "PhaseLabel", Text = "正在启动采集内核…", AutoSize = false, Dock = DockStyle.Fill };
        readonly Label eventCount = Metric("EventCount", "0");
        readonly Label byteCount = Metric("ByteCount", "0 MiB");
        readonly Label gapCount = Metric("GapCount", "0");
        readonly Label elapsed = Metric("ElapsedLabel", "00:00");
        readonly Label footer = new Label { Name = "FooterLabel", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
        readonly DataGridView grid = new DataGridView { Name = "EventsGrid", Dock = DockStyle.Fill };
        readonly RichTextBox details = new RichTextBox { Name = "EventDetails", Dock = DockStyle.Fill, ReadOnly = true, WordWrap = false };
        readonly RichTextBox summaryText = new RichTextBox { Name = "SummaryText", Dock = DockStyle.Fill, ReadOnly = true, WordWrap = true };
        readonly TabControl config = new TabControl { Name = "ConfigTabs", Dock = DockStyle.Fill };
        readonly ComboBox transport = new ComboBox { Name = "TransportInput", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
        readonly ComboBox preset = new ComboBox { Name = "PresetInput", DropDownStyle = ComboBoxStyle.DropDownList, DropDownWidth = 320 };
        readonly Button applyPreset = new Button { Name = "ApplyPresetButton", Text = "一键应用", Enabled = false };
        readonly TextBox presetScriptScope = new TextBox { Name = "PresetScriptScopeInput", Multiline = true, ScrollBars = ScrollBars.Vertical, MaxLength = 8192 };
        readonly RichTextBox presetHint = new RichTextBox { Name = "PresetHint", ReadOnly = true, BorderStyle = BorderStyle.None, BackColor = Color.White, ForeColor = Color.FromArgb(100, 112, 125) };
        readonly Label presetStatus = new Label { Name = "PresetStatus", Text = "当前：自定义 / 手动勾选", TextAlign = ContentAlignment.MiddleLeft, ForeColor = Color.FromArgb(0, 117, 120) };
        readonly Label transportHint = new Label { Name = "TransportHint", Dock = DockStyle.Fill, ForeColor = Color.FromArgb(100, 112, 125) };
        readonly ComboBox casePromptChoice = new ComboBox { Name = "CasePromptChoice", DropDownStyle = ComboBoxStyle.DropDownList, DropDownWidth = 400, Dock = DockStyle.Fill };
        readonly TextBox casePromptGoal = new TextBox { Name = "CasePromptGoal", Multiline = true, ScrollBars = ScrollBars.Vertical, MaxLength = 4096, Dock = DockStyle.Fill };
        readonly TextBox casePromptDirectory = new TextBox { Name = "CasePromptDirectory", Text = "case/SITE_CASE", MaxLength = 1024, Dock = DockStyle.Fill };
        readonly RichTextBox casePromptText = new RichTextBox { Name = "CasePromptText", Dock = DockStyle.Fill, ReadOnly = true, WordWrap = true };
        readonly Label casePromptStatus = new Label { Name = "CasePromptStatus", Text = "正在载入案例提示词…", Dock = DockStyle.Fill, AutoEllipsis = true, ForeColor = Color.FromArgb(0, 117, 120), TextAlign = ContentAlignment.MiddleLeft };
        readonly Button generateCasePrompt = new Button { Name = "GenerateCasePromptButton", Text = "生成 / 更新", Width = 105, Enabled = false };
        readonly Button copyCasePrompt = new Button { Name = "CopyCasePromptButton", Text = "复制全文", Width = 96, Enabled = false };
        readonly Button saveCasePrompt = new Button { Name = "SaveCasePromptButton", Text = "保存提示词…", Width = 122, Enabled = false };
        readonly Dictionary<string, Dictionary<string, bool>> modeSelections = new Dictionary<string, Dictionary<string, bool>>();
        readonly string[] cdpOnly = { "cpuProfile", "contextEvents" };
        string activeTransport = "cdp";
        bool changingTransport, savedRaw;
        bool applyingPreset;
        string activePreset;
        bool ready, busy, closing, exitedReported;
        string captureDirectory;
        string casePromptRequestId;
        DateTime started;
        bool recordingAvailable, recordingReady, recordingPaused, recordingSettled, stoppingCapture;
        long recordingGeneration = -1, recordingMilliseconds = -1;
        string recordingRequestId, captureRequestId;

        public TraceWindow() {
            Text = "V8 Trace Studio · 浏览器采集器 · 作者 Alcohol";
            Name = "V8TraceWindow";
            Size = new Size(1280, 860);
            MinimumSize = new Size(1000, 720);
            StartPosition = FormStartPosition.CenterScreen;
            Font = new Font("Microsoft YaHei UI", 9F);
            BackColor = Color.FromArgb(241, 245, 248);
            AutoScaleMode = AutoScaleMode.Dpi;
            BuildWindow();
            BuildConfigurationTab();
            chrome.Text = FindChrome();
            NewOutput();
            start.Enabled = false;
            timer.Tick += (sender, args) => { Pump(); McpTick(); };
            Shown += (sender, args) => {
                try { worker.Start(Path.GetDirectoryName(typeof(TraceWindow).Assembly.Location)); }
                catch (Exception error) { ShowError(error.Message); }
                timer.Start();
            };
            FormClosing += ClosingWindow;
            FormClosed += (sender, args) => { timer.Stop(); mcpService.Dispose(); worker.Dispose(); };
        }

        static NumericUpDown Number(string name, decimal value, decimal min, decimal max) {
            return new NumericUpDown { Name = name, Minimum = min, Maximum = max, Value = value, ThousandsSeparator = true, Height = 28 };
        }
        static Label Metric(string name, string text) { return new Label { Name = name, Text = text, Dock = DockStyle.Fill, Font = new Font("Segoe UI", 19F, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft }; }
        static string Value(Dictionary<string, object> record, string key) { object value; return record != null && record.TryGetValue(key, out value) && value != null ? Convert.ToString(value) : ""; }
        static long Count(Dictionary<string, object> record, string key) { object value; return record != null && record.TryGetValue(key, out value) ? Convert.ToInt64(value) : 0; }
        static Dictionary<string, object> ObjectValue(Dictionary<string, object> record, string key) { object value; return record != null && record.TryGetValue(key, out value) ? value as Dictionary<string, object> : null; }
        sealed class ScenarioChoice {
            public Dictionary<string, object> Record;
            public override string ToString() { return Value(Record, "label") + (Value(Record, "available") == "False" ? " [专业版]" : ""); }
        }

        void BuildWindow() {
            var root = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 3, ColumnCount = 1, Padding = new Padding(18, 0, 18, 0) };
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 83));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
            var header = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Padding = new Padding(2, 12, 2, 10) };
            header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 60)); header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 40));
            header.Controls.Add(new Label { Text = "V8 TRACE  /  浏览器采集", Font = new Font("Microsoft YaHei UI", 20F, FontStyle.Bold), ForeColor = ink, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 0);
            phase.ForeColor = accent; phase.TextAlign = ContentAlignment.MiddleRight; header.Controls.Add(phase, 1, 0);
            root.Controls.Add(header, 0, 0);
            var body = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2 };
            body.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 365)); body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            BuildConfig();
            var left = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2, ColumnCount = 1 };
            left.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); left.RowStyles.Add(new RowStyle(SizeType.Absolute, 54));
            left.Controls.Add(config, 0, 0); left.Controls.Add(BuildCaptureActions(), 0, 1);
            body.Controls.Add(left, 0, 0); body.Controls.Add(BuildEvidence(), 1, 0);
            root.Controls.Add(body, 0, 1);
            footer.Text = "CDP / 非 CDP 双模式 · 原生执行与网络诊断分别注明记录范围";
            footer.ForeColor = Color.FromArgb(83, 100, 116); root.Controls.Add(footer, 0, 2); Controls.Add(root);
        }

        TableLayoutPanel SettingsPanel(TabPage tab) {
            tab.BackColor = Color.White;
            var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true, Padding = new Padding(12) };
            var table = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 1, Padding = new Padding(0, 0, 7, 10) };
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); scroll.Controls.Add(table); tab.Controls.Add(scroll); return table;
        }
        void Row(TableLayoutPanel table, Control control, int height) {
            int index = table.RowCount++; table.RowStyles.Add(new RowStyle(SizeType.Absolute, height)); control.Dock = DockStyle.Fill; table.Controls.Add(control, 0, index);
        }
        void Field(TableLayoutPanel table, string label, Control input, Action browse = null) {
            Row(table, new Label { Text = label, ForeColor = ink, TextAlign = ContentAlignment.BottomLeft }, 28);
            if (browse == null) { Row(table, input, 32); return; }
            var row = new TableLayoutPanel { ColumnCount = 2, Dock = DockStyle.Fill, Margin = new Padding(0) };
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 58)); input.Dock = DockStyle.Fill; row.Controls.Add(input, 0, 0);
            var button = new Button { Text = "浏览", Dock = DockStyle.Fill, Margin = new Padding(4, 0, 0, 0) }; button.Click += (sender, args) => browse(); row.Controls.Add(button, 1, 0); Row(table, row, 32);
        }
        void Channel(TableLayoutPanel table, string key, string text, bool enabled) {
            var box = new CheckBox { Name = "Channel_" + key, Text = text, Checked = enabled, AutoSize = true };
            channels.Add(key, box); Row(table, box, 28);
        }
        void BuildConfig() {
            var basic = new TabPage("采集配置"); var advanced = new TabPage("高级选项"); var detailTab = new TabPage("调用过滤") { Name = "DetailFilterTab" }; config.TabPages.Add(basic); config.TabPages.Add(advanced); config.TabPages.Add(detailTab);
            var tcpTab = new TabPage("TCP 日志传输") { Name = "TcpTransferTab" }; config.TabPages.Add(tcpTab);
            var tcpSettings = SettingsPanel(tcpTab);
            Row(tcpSettings, new Label { Text = "已落盘采集包 → TCP 接收端。使用 Zstd 无损压缩、4 MiB 分块、SHA-256 校验、断线续传；不减少字段或采样。", ForeColor = ink }, 90);
            Row(tcpSettings, new Label { Text = "接收端地址", AutoSize = true }, 24); Row(tcpSettings, tcpHost, 30);
            Row(tcpSettings, new Label { Text = "端口", AutoSize = true }, 24); Row(tcpSettings, tcpPort, 30);
            tcpPort.ThousandsSeparator = false;
            Row(tcpSettings, new Label { Text = "令牌文件（跨机器传输必须填写，同接收端）", AutoSize = true }, 24); Row(tcpSettings, tcpToken, 30);
            Row(tcpSettings, tcpAfterCapture, 48); Row(tcpSettings, tcpSend, 40); Row(tcpSettings, tcpStatus, 70);
            Row(tcpSettings, chunkSpool, 40); Row(tcpSettings, streamTCP, 40);
            streamTCP.CheckedChanged += (sender, args) => { if (streamTCP.Checked) chunkSpool.Checked = true; };
            Row(tcpSettings, new Label { Text = "分块默认放到日志目录同级的 .stream 目录；zstd 完整保存，离线或断线保留 spool，停止后仍可续传。原生浏览器可能停止时才刷出 trace，此选项是宿主产生后的持续搬运，不是 V8 生产线程 TCP。接收分块与最终提交分别显示。", ForeColor = ink }, 140);
            Row(tcpSettings, new Label { Text = "接收端命令：\nruntime/v8trace-runtime receive --root ./received --bind 127.0.0.1 --port 17880\n\n跨机器时指定监听地址和 --token-file。TCP 是明文通道，请放在可信网络或加密隧道内。浏览器 profile 不传输，完整原始 trace、日志和正文均传输。当前是保存后传输，不是 Chromium 直接推流。", ForeColor = ink }, 240);
            tcpSend.Click += (sender, args) => SendTcp();
            var settings = SettingsPanel(basic);
            Row(settings, new Label { Text = "场景组合（选择后点击一键应用）", ForeColor = ink, TextAlign = ContentAlignment.BottomLeft }, 28);
            var presetRow = new TableLayoutPanel { ColumnCount = 2, Margin = new Padding(0) };
            presetRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); presetRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 85));
            preset.Dock = DockStyle.Fill; applyPreset.Dock = DockStyle.Fill; presetRow.Controls.Add(preset, 0, 0); presetRow.Controls.Add(applyPreset, 1, 0); Row(settings, presetRow, 32);
            preset.Items.Add("自定义 / 手动勾选"); preset.SelectedIndex = 0;
            Row(settings, new Label { Text = "定向组合：目标脚本 URL / 文件名 glob（每行一条）", ForeColor = ink, TextAlign = ContentAlignment.BottomLeft }, 28);
            Row(settings, presetScriptScope, 55);
            presetScriptScope.TextChanged += (sender, args) => ShowPreset();
            Row(settings, presetStatus, 36); Row(settings, presetHint, 112);
            preset.SelectedIndexChanged += (sender, args) => ShowPreset();
            applyPreset.Click += (sender, args) => ApplyPreset();
            ShowPreset();
            transport.Items.AddRange(new object[] { "CDP 采集（现有完整通道）", "原生采集（无 CDP）" }); transport.SelectedIndex = 0;
            Field(settings, "采集方式", transport);
            Row(settings, transportHint, 66);
            Field(settings, "目标网址", url);
            Field(settings, "Chrome 可执行文件", chrome, () => { using (var dialog = new OpenFileDialog { Filter = "Chrome 程序|*.exe", FileName = String.IsNullOrWhiteSpace(chrome.Text) ? "" : ResolveLocalPath(chrome.Text) }) if (dialog.ShowDialog(this) == DialogResult.OK) chrome.Text = DisplayLocalPath(dialog.FileName); });
            Field(settings, "日志输出（新目录）", output, () => { using (var dialog = new FolderBrowserDialog { Description = "选择日志根目录，将自动创建新的采集目录" }) if (dialog.ShowDialog(this) == DialogResult.OK) output.Text = DisplayLocalPath(Path.Combine(dialog.SelectedPath, "capture-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff"))); });
            Row(settings, new Label { Text = @"相对路径基于 V8Trace.exe 所在目录；默认使用 .\chromium 与 .\logs，换目录后自动跟随。", ForeColor = Color.FromArgb(100, 112, 125) }, 36);
            Field(settings, "采集时长（秒）", duration);
            Row(settings, recordingControl, 30); Row(settings, startPaused, 30);
            Row(settings, new Label { Text = "暂停只停止记录，不暂停网页；可反复恢复，暂停时间不计入采集时长。非 CDP 需新版定制 Chromium。恢复不补录暂停期间日志。", ForeColor = Color.FromArgb(100, 112, 125) }, 66);
            recordingControl.CheckedChanged += (sender, args) => { startPaused.Enabled = !busy && recordingControl.Checked; if (!recordingControl.Checked) startPaused.Checked = false; };
            Row(settings, new Label { Text = "选择采集项目", ForeColor = accent, Font = new Font(Font, FontStyle.Bold), TextAlign = ContentAlignment.BottomLeft }, 36);
            Channel(settings, "http", "HTTP / 请求与响应 body", true);
            Channel(settings, "redirectBodies", "重定向正文补读 / 定制浏览器（最多 5 秒）", false);
            Channel(settings, "netLog", "网络诊断 / 原始 NetLog（非 CDP）", false);
            Channel(settings, "domTrace", "DOM 原生解析 / 节点、属性、查询与变更证据", false);
            Channel(settings, "cryptoTrace", "加密关键点 / WebCrypto 复制输入与异步结果", false);
            Channel(settings, "httpRootTrace", "HTTP 首栈全链 / 同步、异步、Worker 与消息因果", false);
            Channel(settings, "scripts", "脚本源码 / eval / 动态脚本", true);
            Channel(settings, "hooks", "Env / API 参数、返回、异常（Hook）", false);
            Channel(settings, "cpuProfile", "JSCall / CPU 调用采样", true);
            Channel(settings, "nativeTimeline", "Native / V8、Blink 加载与绘制时间线", true);
            Channel(settings, "contextEvents", "Frame / Worker / 上下文生命周期", true);
            Channel(settings, "consoleEvents", "Console / 控制台日志", true);
            Channel(settings, "exceptionEvents", "Exception / 未处理异常", true);
            Channel(settings, "jsCallDetail", "JSCall / 逐次调用结构（详情在调用过滤页选择）", false);
            Channel(settings, "nativeDetail", "Native / API、Runtime、builtin 参数", false);
            Channel(settings, "webSocket", "WebSocket / 原生收发内容与连接事件", false);
            Channel(settings, "indexedDB", "IndexedDB / 原生事务、请求、键与序列化值", false);
            Channel(settings, "cookies", "Cookie / JS 可见读写与原生存储变更", false);
            Channel(settings, "storage", "Storage / 原生读写、删除及跨窗口通知", false);
            foreach (var key in new[] { "webSocket", "indexedDB", "cookies", "storage" }) channels[key].CheckedChanged += (sender, args) => { if (!applyingPreset && channels[key].Checked) { var native = FindOpcodeChrome(); if (native != null) chrome.Text = native; } };
            Row(settings, new Label { Text = "以上四个专项通道在 CDP / 非 CDP 模式均由定制内核记录，不替换页面方法。WebSocket 是解码后内容；外部 Blob/文件句柄缺口单独记录。", ForeColor = Color.FromArgb(100, 112, 125) }, 66);
            Channel(settings, "wasm", "WASM / 实际指令与带类型栈值", false);
            Channel(settings, "wasmModules", "WASM 模块字节 / 动态与网络输入（不强制 DrumBrake）", false);
            Channel(settings, "objectSnapshots", "对象快照 / 自有属性、数组及访问器描述", false);
            Channel(settings, "correlations", "关联证据 / 调用、源码与网络（含边界）", false);
            foreach (string key in new[] { "jsCallDetail", "nativeDetail", "wasm", "wasmModules" }) channels[key].CheckedChanged += (sender, args) => { if (!applyingPreset && channels[key].Checked) { var native = FindOpcodeChrome(); if (native != null) chrome.Text = native; } };
            channels["redirectBodies"].CheckedChanged += (sender, args) => { if (!applyingPreset && channels["redirectBodies"].Checked) { var native = FindOpcodeChrome(); if (native != null) chrome.Text = native; } };
            foreach (string key in new[] { "http", "scripts", "hooks", "consoleEvents", "exceptionEvents" }) channels[key].CheckedChanged += (sender, args) => { if (!applyingPreset && activeTransport == "native" && channels[key].Checked) { var native = FindOpcodeChrome(); if (native != null) chrome.Text = native; } };
            Channel(settings, "opcode", "Opcode / 原生 Ignition 指令与寄存器", false);
            channels["opcode"].CheckedChanged += (sender, args) => { if (!applyingPreset && channels["opcode"].Checked) { var native = FindOpcodeChrome(); if (native != null) chrome.Text = native; } };
            Row(settings, new Label { Text = "Opcode、逐次 JSCall、Native 参数及 WASM 指令需对应定制 Chromium。原生诊断关闭 JS 优化；WASM 使用 DrumBrake。采样与性能事件为独立选项。", ForeColor = Color.FromArgb(114, 80, 38) }, 64);
            var options = SettingsPanel(advanced);
            Row(options, headless, 34); Row(options, raw, 34);
            Field(options, "事件预算（条）", maxEvents); Field(options, "日志 / blob 预算（MiB）", maxMiB); Field(options, "单个 body 上限（MiB）", bodyMiB);
            Field(options, "宿主日志写缓冲（MiB）", writeBufferMiB); Field(options, "宿主连续日志批量（KiB）", writeBatchKiB);
            Row(options, new Label { Text = "缓冲满时等待写盘，不采样或丢行；单条超大日志保持完整并独占队列。停止会排空缓冲。这里是宿主日志缓冲，不是浏览器 Perfetto 缓冲，也不是实时 TCP 推流。", ForeColor = Color.FromArgb(100, 112, 125) }, 95);
            Field(options, "停止条件表达式（可留空）", until);
            Field(options, "对象快照最大层数", objectDepth);
            Field(options, "单个值图对象节点上限", objectNodes);
            Field(options, "单个值图属性 / 元素上限", objectProperties);
            Row(options, new Label { Text = "对象快照搭配逐次 JSCall、Native 或 WASM。读取自有存储，不执行 getter / Proxy；达到值图预算会标记边界，调用与 Opcode 记录保持独立。", ForeColor = Color.FromArgb(100, 112, 125) }, 80);
            Field(options, "案例导出新目录（可留空）", exportOutput);
            Row(options, new Label { Text = "表达式轮询返回 true 后停止。Header、Cookie 和字符串原样记录。预算耗尽会明确保存缺口；界面预览截断不影响完整磁盘记录。", ForeColor = Color.FromArgb(100, 112, 125) }, 80);
            transport.SelectedIndexChanged += (sender, args) => ChangeTransport();
            BuildEnvironmentProbes();
            BuildVmpProbes();
            foreach (var box in channels.Values) box.CheckedChanged += (sender, args) => CustomizePreset();
            raw.CheckedChanged += (sender, args) => CustomizePreset();
            foreach (var number in PresetNumbers().Values) number.ValueChanged += (sender, args) => CustomizePreset();
            writeBufferMiB.ValueChanged += (sender, args) => CustomizePreset(); writeBatchKiB.ValueChanged += (sender, args) => CustomizePreset();
            ApplyTransportAvailability();
            BuildDetailFilters(detailTab);
        }

        void BuildDetailFilters(TabPage tab) {
            var panel = SettingsPanel(tab);
            Row(panel, jsCallValues, 34);
            Row(panel, closureBindings, 34);
            Row(panel, new Label { Text = "闭包默认关闭，必须指定具体脚本或函数白名单；与参数值开关独立。按入口、返回与 generator 激活段取当前捕获绑定，不执行 getter。栈绑定、动态/模块边界和预算会明确标记。", ForeColor = ink }, 90);
            Field(panel, "闭包作用域上限", closureScopes); Field(panel, "闭包绑定总上限", closureBindingsLimit);
            Field(panel, "闭包值图层数", closureDepth); Field(panel, "闭包值图节点数", closureNodes); Field(panel, "闭包值图属性数", closureProperties);
            closureBindings.CheckedChanged += (sender, args) => { if (!applyingPreset && closureBindings.Checked) { channels["jsCallDetail"].Checked = true; detailFiltering.Checked = true; } CustomizePreset(); };
            foreach (var number in new[] { closureScopes, closureBindingsLimit, closureDepth, closureNodes, closureProperties }) number.ValueChanged += (sender, args) => CustomizePreset();
            Row(panel, new Label { Text = "关闭详情后仍逐次记录入口、退出和函数来源，不读取调用参数值。保持勾选且详情范围留空时，仍按原设置完整采集。分层模式需要新版定制 Chromium；旧版会明确提示版本不匹配。", ForeColor = ink }, 85);
            Row(panel, detailFiltering, 32);
            Row(panel, new Label { Text = "在 V8 构造参数日志 / 对象快照之前过滤，不是右侧的显示筛选。匹配调用保留完整实参、返回与异常；需要新版定制 Chromium。", ForeColor = ink }, 68);
            string[] keys = { "detailScriptInclude", "detailScriptExclude", "jsFunctionInclude", "jsFunctionExclude", "nativeNameInclude", "nativeNameExclude", "jsValueScriptInclude", "jsValueScriptExclude", "jsValueFunctionInclude", "jsValueFunctionExclude" };
            string[] labels = { "调用范围：脚本 URL / 文件名白名单（每行一条 glob）", "调用范围：脚本黑名单（优先于白名单）", "调用范围：JS 函数名白名单（留空全部）", "调用范围：JS 函数名黑名单", "Native 名称白名单（留空全部）", "Native 名称黑名单", "仅 JS 详情：脚本白名单（其余调用仍记录结构）", "仅 JS 详情：脚本黑名单", "仅 JS 详情：函数名白名单（留空全部）", "仅 JS 详情：函数名黑名单" };
            for (int index = 0; index < keys.Length; index++) {
                var input = new TextBox { Name = keys[index] + "Input", Multiline = true, ScrollBars = ScrollBars.Vertical, MaxLength = 8192 };
                detailRules.Add(keys[index], input);
                Row(panel, new Label { Text = labels[index], ForeColor = ink, TextAlign = ContentAlignment.BottomLeft }, 28);
                Row(panel, input, 55); input.TextChanged += (sender, args) => CustomizePreset();
            }
            Row(panel, new Label { Text = "Native 子类（独立选择）", ForeColor = ink, TextAlign = ContentAlignment.BottomLeft }, 28);
            Row(panel, nativeKinds, 145);
            Row(panel, detailExcludeInternal, 32);
            detailUnknownSource.Items.AddRange(new object[] { "排除来源未知", "保留来源未知" }); detailUnknownSource.SelectedIndex = 0;
            Field(panel, "来源未知的 Native / 动态代码", detailUnknownSource);
            Row(panel, new Label { Text = "支持 *、? 通配符；文件名规则如 target.js 或 *target*.js，URL 规则如 https://host/path/*.js*。Native 按真实 JS 调用方匹配；此处不替代动态代码来源分析。未勾选启用时保持原全量采集。", ForeColor = Color.FromArgb(100, 112, 125) }, 95);
            detailFiltering.CheckedChanged += (sender, args) => { UpdateDetailFilters(); CustomizePreset(); };
            jsCallValues.CheckedChanged += (sender, args) => { UpdateDetailFilters(); CustomizePreset(); };
            channels["jsCallDetail"].CheckedChanged += (sender, args) => UpdateDetailFilters();
            detailExcludeInternal.CheckedChanged += (sender, args) => CustomizePreset();
            detailUnknownSource.SelectedIndexChanged += (sender, args) => CustomizePreset();
            nativeKinds.ItemCheck += (sender, args) => CustomizePreset();
            UpdateDetailFilters();
        }

        void BuildVmpProbes() {
            var tab = new TabPage("已确认 VM 探针") { Name = "VmpProbeTab" }; config.TabPages.Add(tab); var panel = SettingsPanel(tab);
            Channel(panel, "vmpProbe", "启用明确确认后的原生 JSVM 状态点位", false);
            Row(panel, new Label { Text = "先通过 Opcode / vmp-discover 定位候选，再核对 dispatcher、点位和活跃存储。描述必须 confirmed=true，并同时核验原始 UTF-16 源码及函数字节码 SHA-256；不猜未知 VM 的 opcode 语义，也不改原脚本。", ForeColor = ink }, 110);
            Field(panel, "已确认描述文件（UTF-8 JSON）", vmpProbeFile, () => { using (var dialog = new OpenFileDialog { Filter = "VM 描述|*.json" }) if (dialog.ShowDialog(this) == DialogResult.OK) vmpProbeFile.Text = DisplayLocalPath(dialog.FileName); });
            Field(panel, "值图最大层数", vmpDepth); Field(panel, "值图节点数", vmpNodes); Field(panel, "值图属性数", vmpProperties); Field(panel, "TypedArray 单次字节预算（MiB）", vmpTypedMiB); Field(panel, "活跃帧预算", vmpMaxActive);
            Row(panel, new Label { Text = "记录虚拟 PC / opcode / 已确认的操作数、栈和寄存器，支持参数、解释器寄存器及 Context 槽。异常、错版/错点位和预算缺口会单独记录。该模式保持解释执行，但无需同时开启全量 Opcode；仍有逐指令检查开销。", ForeColor = ink }, 125);
            channels["vmpProbe"].CheckedChanged += (sender, args) => { if (channels["vmpProbe"].Checked) { channels["scripts"].Checked = true; var native = FindOpcodeChrome(); if (native != null) chrome.Text = native; } };
        }
        void BuildEnvironmentProbes() {
            var tab = new TabPage("原生环境取证") { Name = "NativeEnvironmentTab" }; config.TabPages.Add(tab);
            var panel = SettingsPanel(tab);
            Row(panel, new Label { Text = "独立于 Env Hook，不替换页面函数身份。为覆盖缓存命中，使用 Ignition 解释执行并关闭 JS 优化；默认关闭，建议指定目标脚本，先做低开销基线。", ForeColor = ink }, 90);
            Channel(panel, "envAbsentProbe", "缺失属性候选 / undefined 与真实存在状态分开记录", false);
            Channel(panel, "envInterfaceProbe", "接口探测 / 读取、in、hasOwn、描述符", false);
            Channel(panel, "envHostMutation", "宿主改动 / 属性赋值、定义、删除与原型设置", false);
            Channel(panel, "envInventory", "接口清单 / 当前 realm 的原始命名存储", false);
            Row(panel, new Label { Text = "库存不执行 getter、Proxy 或惰性构造器，不展开元素与虚拟属性；动态宿主的存在状态保留 unknown，绝不将 undefined 直接当作缺失。", ForeColor = ink }, 85);
            Field(panel, "库存命名属性预算", envInventoryProperties);
            Row(panel, new Label { Text = "独立脚本白名单（每行一条 glob，留空全部）" }, 30); Row(panel, envScriptInclude, 65);
            Row(panel, new Label { Text = "独立脚本黑名单（优先）" }, 30); Row(panel, envScriptExclude, 65);
            Row(panel, envExcludeInternal, 32);
            envUnknownSource.Items.AddRange(new object[] { "保留来源未知", "排除来源未知" }); envUnknownSource.SelectedIndex = 0;
            Field(panel, "来源未知处理", envUnknownSource);
            foreach (var key in new[] { "envAbsentProbe", "envInterfaceProbe", "envHostMutation", "envInventory" }) channels[key].CheckedChanged += (sender, args) => { if (!applyingPreset) { var native = FindOpcodeChrome(); if (native != null) chrome.Text = native; } };
            envScriptInclude.TextChanged += (sender, args) => CustomizePreset(); envScriptExclude.TextChanged += (sender, args) => CustomizePreset();
            envUnknownSource.SelectedIndexChanged += (sender, args) => CustomizePreset(); envExcludeInternal.CheckedChanged += (sender, args) => CustomizePreset(); envInventoryProperties.ValueChanged += (sender, args) => CustomizePreset();
        }
        void UpdateDetailFilters() {
            detailFiltering.Enabled = !busy;
            jsCallValues.Enabled = channels["jsCallDetail"].Checked && !busy;
            foreach (var item in detailRules) item.Value.Enabled = detailFiltering.Checked && !busy && (!item.Key.StartsWith("jsValue", StringComparison.Ordinal) || (channels["jsCallDetail"].Checked && jsCallValues.Checked));
            foreach (Control input in new Control[] { nativeKinds, detailExcludeInternal, detailUnknownSource }) input.Enabled = detailFiltering.Checked && !busy;
        }

        Dictionary<string, NumericUpDown> PresetNumbers() {
            return new Dictionary<string, NumericUpDown> { { "durationSeconds", duration }, { "maxEvents", maxEvents }, { "maxBytes", maxMiB }, { "maxBodyBytes", bodyMiB },
                { "objectDepth", objectDepth }, { "objectNodes", objectNodes }, { "objectProperties", objectProperties }, { "writeBufferBytes", writeBufferMiB }, { "writeBatchBytes", writeBatchKiB } };
        }
        static bool HasConcreteScriptScope(string value) {
            var rules = value.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries).Select(line => line.Trim()).Where(line => line.Length > 0).ToArray();
            return rules.Length > 0 && rules.All(rule => rule.Any(ch => ch != '*' && ch != '?' && !Char.IsWhiteSpace(ch)));
        }
        static bool RequiresScriptScope(ScenarioChoice choice) { return choice != null && Value(choice.Record, "requiresScriptScope") == "True"; }
        void ShowPreset() {
            var choice = preset.SelectedItem as ScenarioChoice;
            applyPreset.Enabled = ready && !busy && PresetAvailable(choice) && (!RequiresScriptScope(choice) || HasConcreteScriptScope(presetScriptScope.Text));
            presetHint.Text = choice == null ? "保留当前设置，逐项勾选通道。选择预设只预览说明，不会立即覆盖配置。应用预设不启动采集，也不执行案例代码。" :
                "阶段：" + Value(choice.Record, "stage") + " / 开销：" + Value(choice.Record, "risk") + "\n" + Value(choice.Record, "purpose") + "\n\n要求：" + Value(choice.Record, "requirement") + "\n" + Value(choice.Record, "outputs") + "\n\n推荐：" + Count(ObjectValue(choice.Record, "options"), "durationSeconds") + " 秒 / " + (Count(ObjectValue(choice.Record, "options"), "maxBytes") / 1048576) + " MiB。应用后仍可逐项调整。" +
                (RequiresScriptScope(choice) ? "\n必须填写具体目标脚本范围；修改目标后重新应用。仅约束 JSCall/Native 与定向闭包，不约束 Env Hook、原生环境专项、HTTP、Opcode 或 WASM。" : "");
            if (!PresetAvailable(choice)) presetHint.Text = "此组合包含 Professional 专业采集项；当前配置保持原样。Community 的两种补环境组合完整可用。\n\n" + presetHint.Text;
        }
        void ApplyPreset() {
            if (!ready || busy) return;
            var choice = preset.SelectedItem as ScenarioChoice;
            if (choice == null) { activePreset = null; presetStatus.Text = "当前：自定义 / 手动勾选（设置保留）"; return; }
            if (!PresetAvailable(choice)) { presetStatus.Text = "该组合属于 Professional；当前配置未应用。"; return; }
            if (RequiresScriptScope(choice) && !HasConcreteScriptScope(presetScriptScope.Text)) { presetStatus.Text = "请先填写具体目标脚本；空范围及全局通配规则不生效。"; return; }
            applyingPreset = true;
            try {
                importedOptions = null; importedSnapshot = null;
                var options = ObjectValue(choice.Record, "options");
                transport.SelectedIndex = Value(options, "transport") == "native" ? 1 : 0;
                foreach (var item in channels) item.Value.Checked = options.ContainsKey(item.Key) && Convert.ToBoolean(options[item.Key]);
                chunkSpool.Checked = options.ContainsKey("chunkSpool") && Convert.ToBoolean(options["chunkSpool"]);
                streamTCP.Checked = options.ContainsKey("streamTCP") && Convert.ToBoolean(options["streamTCP"]);
                foreach (var item in StudioNumbers().Where(entry => entry.Key.StartsWith("vmp", StringComparison.Ordinal))) if (options.ContainsKey(item.Key)) item.Value.Value = Convert.ToDecimal(options[item.Key]) / (item.Key == "vmpTypedArrayBytes" ? 1048576 : 1);
                envScriptInclude.Text = Value(options, "envScriptInclude"); envScriptExclude.Text = Value(options, "envScriptExclude");
                envExcludeInternal.Checked = options.ContainsKey("envExcludeInternal") && Convert.ToBoolean(options["envExcludeInternal"]);
                envUnknownSource.SelectedIndex = Value(options, "envUnknownSource") == "exclude" ? 1 : 0;
                envInventoryProperties.Value = options.ContainsKey("envInventoryProperties") ? Convert.ToDecimal(options["envInventoryProperties"]) : 65536;
                closureBindings.Checked = options.ContainsKey("closureBindings") && Convert.ToBoolean(options["closureBindings"]);
                closureScopes.Value = options.ContainsKey("closureScopes") ? Convert.ToDecimal(options["closureScopes"]) : 32;
                closureBindingsLimit.Value = options.ContainsKey("closureBindingsLimit") ? Convert.ToDecimal(options["closureBindingsLimit"]) : 4096;
                closureDepth.Value = options.ContainsKey("closureDepth") ? Convert.ToDecimal(options["closureDepth"]) : 2;
                closureNodes.Value = options.ContainsKey("closureNodes") ? Convert.ToDecimal(options["closureNodes"]) : 256;
                closureProperties.Value = options.ContainsKey("closureProperties") ? Convert.ToDecimal(options["closureProperties"]) : 4096;
                raw.Checked = Convert.ToBoolean(options["rawCdp"]);
                foreach (var item in PresetNumbers()) {
                    decimal value = Convert.ToDecimal(options[item.Key]);
                    if (item.Key == "maxBytes" || item.Key == "maxBodyBytes" || item.Key == "writeBufferBytes") value /= 1048576;
                    if (item.Key == "writeBatchBytes") value /= 1024;
                    item.Value.Value = value;
                }
                jsCallValues.Checked = Convert.ToBoolean(options["jsCallValues"]);
                foreach (var item in detailRules) item.Value.Text = Value(options, item.Key);
                if (RequiresScriptScope(choice)) detailRules["detailScriptInclude"].Text = presetScriptScope.Text.Trim();
                detailExcludeInternal.Checked = Convert.ToBoolean(options["detailExcludeInternal"]);
                detailUnknownSource.SelectedIndex = Value(options, "detailUnknownSource") == "exclude" ? 0 : 1;
                var kinds = Value(options, "nativeKinds").Split(',');
                for (int index = 0; index < nativeKinds.Items.Count; index++) nativeKinds.SetItemChecked(index, kinds.Contains("all") || kinds.Contains(Value(((ScenarioChoice)nativeKinds.Items[index]).Record, "id")));
                detailFiltering.Checked = detailRules.Values.Any(input => !String.IsNullOrWhiteSpace(input.Text)) || detailExcludeInternal.Checked || detailUnknownSource.SelectedIndex == 0 || !kinds.Contains("all");
                if (activeTransport == "native") { var native = FindOpcodeChrome(); if (native != null) chrome.Text = native; }
                activePreset = Value(choice.Record, "id");
                ApplyTransportAvailability();
                UpdateDetailFilters();
                presetStatus.Text = "已应用：" + choice.ToString();
                summaryText.Text = "场景：" + choice.ToString() + "\n\n" + presetHint.Text + "\n\n已完整替换通道、调用范围、JSCall 值模式、Native 子类、对象及写入预算；导入配置中的未呈现高级字段不继承。网址、日志/案例目录、无头模式和表达式保留。点击开始采集后生效，结束后手动导出案例。";
            } catch (Exception error) { activePreset = null; presetStatus.Text = "预设应用错误 · 请检查当前选项"; ShowError(error.Message); }
            finally { applyingPreset = false; }
        }
        void CustomizePreset() {
            if (applyingPreset || changingTransport || busy || activePreset == null) return;
            presetStatus.Text = "当前：自定义（基于 " + activePreset + "）";
        }

        void ChangeTransport() {
            if (changingTransport || busy) return;
            changingTransport = true;
            try {
                modeSelections[activeTransport] = channels.ToDictionary(item => item.Key, item => item.Value.Checked);
                if (activeTransport == "cdp") savedRaw = raw.Checked;
                activeTransport = transport.SelectedIndex == 1 ? "native" : "cdp";
                Dictionary<string, bool> saved;
                if (modeSelections.TryGetValue(activeTransport, out saved)) {
                    foreach (var item in channels) item.Value.Checked = saved[item.Key];
                } else {
                    foreach (var item in channels) item.Value.Checked = item.Key == "nativeTimeline" || item.Key == "netLog";
                }
                raw.Checked = activeTransport == "cdp" && savedRaw;
                ApplyTransportAvailability();
            } finally { changingTransport = false; }
            CustomizePreset();
        }

        void ApplyTransportAvailability() {
            bool native = activeTransport == "native";
            foreach (var item in channels) {
                bool supported = (item.Key == "netLog" || item.Key == "httpRootTrace" || item.Key == "domTrace" || item.Key == "cryptoTrace") ? native : !native || !cdpOnly.Contains(item.Key);
                if (!supported) item.Value.Checked = false;
                item.Value.Enabled = !busy && supported;
            }
            raw.Enabled = !busy && !native; until.Enabled = !busy && !native;
            transportHint.Text = native
                ? "无调试端口、无 CDP 连接。定制 Chromium 支持 HTTP 正文、源码、Env Hook、Console 和异常；Hook 会改变函数身份。CPU 采样 / 生命周期仍使用 CDP。未确认刷盘标为部分完成。"
                : "使用 CDP 控制导航与读取日志；定制 Chromium 另支持完整 Opcode、逐次 JSCall、Native 参数和 WASM。";
            if (!advancedAnalysis) transportHint.Text = native
                ? "Community 无 CDP；完整补环境及目标 JSCall/Native。需配套社区 Chromium；Hook 改变函数身份。[专业版] 项未编译。"
                : "Community CDP 模式；目标 JSCall/Native 完整值保留。原生专项需社区 Chromium；[专业版] 为未编译采集项。";
            ApplyEditionAvailability();
        }

        Control BuildCaptureActions() {
            var actions = new TableLayoutPanel { ColumnCount = 3, Dock = DockStyle.Fill, Margin = new Padding(0), Padding = new Padding(0, 8, 0, 0) };
            for (int index = 0; index < 3; index++) actions.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F / 3));
            start.Dock = DockStyle.Fill; stop.Dock = DockStyle.Fill; start.BackColor = accent; start.ForeColor = Color.White; start.FlatStyle = FlatStyle.Flat; start.FlatAppearance.BorderSize = 0;
            pauseRecording.Dock = DockStyle.Fill; actions.Controls.Add(start, 0, 0); actions.Controls.Add(pauseRecording, 1, 0); actions.Controls.Add(stop, 2, 0);
            start.Click += (sender, args) => StartCapture(); stop.Click += (sender, args) => Send("stop");
            pauseRecording.Click += (sender, args) => ToggleRecording();
            return actions;
        }

        Control BuildEvidence() {
            var right = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 4, ColumnCount = 1, Padding = new Padding(12, 0, 0, 0) };
            right.RowStyles.Add(new RowStyle(SizeType.Absolute, 91)); right.RowStyles.Add(new RowStyle(SizeType.Absolute, 42)); right.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); right.RowStyles.Add(new RowStyle(SizeType.Absolute, 45));
            var metrics = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 4, BackColor = Color.White, Padding = new Padding(12, 7, 12, 7) };
            string[] names = { "规范化事件", "已记录数据", "关键缺口", "运行时间" }; Label[] labels = { eventCount, byteCount, gapCount, elapsed };
            for (int i = 0; i < 4; i++) {
                metrics.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 25));
                var cell = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2, Margin = new Padding(6, 0, 6, 0) }; cell.RowStyles.Add(new RowStyle(SizeType.Absolute, 24)); cell.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
                cell.Controls.Add(new Label { Text = names[i], Dock = DockStyle.Fill, ForeColor = Color.FromArgb(97, 111, 126) }, 0, 0); labels[i].ForeColor = ink; cell.Controls.Add(labels[i], 0, 1); metrics.Controls.Add(cell, i, 0);
            }
            right.Controls.Add(metrics, 0, 0);
            var tools = new FlowLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(0, 7, 0, 0), WrapContents = false };
            category.Items.AddRange(new object[] { "全部", "http", "script", "env", "jscall", "native", "opcode", "wasm", "lifecycle", "console", "exception", "websocket", "indexeddb", "cookie", "storage", "wasm-module", "vmp" }); category.SelectedIndex = 0;
            tools.Controls.Add(category); tools.Controls.Add(new Label { Text = "筛选", AutoSize = true, Margin = new Padding(8, 7, 3, 0) }); tools.Controls.Add(filter); tools.Controls.Add(refresh);
            category.SelectedIndexChanged += (sender, args) => RenderEvents(); filter.TextChanged += (sender, args) => RenderEvents(); refresh.Click += (sender, args) => ReadEvents(); right.Controls.Add(tools, 0, 1);
            var tabs = new TabControl { Dock = DockStyle.Fill, Name = "EvidenceTabs" }; var logTab = new TabPage("实时事件 / 详情"); var summaryTab = new TabPage("采集摘要 / 缺口");
            var split = new SplitContainer { Dock = DockStyle.Fill, Size = new Size(700, 520), Orientation = Orientation.Horizontal, SplitterDistance = 290, Panel1MinSize = 160, Panel2MinSize = 90 };
            ConfigureGrid(); split.Panel1.Controls.Add(grid); details.Font = new Font("Consolas", 9F); details.BackColor = Color.FromArgb(248, 250, 252); split.Panel2.Controls.Add(details); logTab.Controls.Add(split);
            summaryText.Font = new Font("Microsoft YaHei UI", 10F); summaryTab.Controls.Add(summaryText); tabs.TabPages.Add(logTab); tabs.TabPages.Add(summaryTab); tabs.TabPages.Add(BuildIndexedTab()); tabs.TabPages.Add(BuildCausalityTab()); tabs.TabPages.Add(BuildMcpTab()); tabs.TabPages.Add(BuildCasePromptTab()); right.Controls.Add(tabs, 0, 2);
            var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(0, 7, 0, 0), WrapContents = false }; actions.Controls.Add(load); actions.Controls.Add(open); actions.Controls.Add(export);
            load.Click += (sender, args) => LoadCapture(); open.Click += (sender, args) => OpenDirectory(); export.Click += (sender, args) => ExportCapture(); right.Controls.Add(actions, 0, 3);
            return right;
        }
        TabPage BuildCasePromptTab() {
            var tab = new TabPage("案例提示词") { Name = "CasePromptTab", BackColor = Color.White };
            var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5, Padding = new Padding(9) };
            panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 38)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 104));
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 38)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 26)); panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            panel.Controls.Add(new Label { Dock = DockStyle.Fill, Text = "浏览器只用于取证；最终复现禁止自动化方案 / 兜底。\n网址沿用左侧配置；下方内容交给 AI 执行，不会自动启动任务。", ForeColor = ink }, 0, 0);
            var inputs = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 3, Margin = new Padding(0) };
            inputs.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 85)); inputs.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            inputs.RowStyles.Add(new RowStyle(SizeType.Absolute, 31)); inputs.RowStyles.Add(new RowStyle(SizeType.Absolute, 31)); inputs.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            string[] labels = { "任务路线", "案例目录", "复现目标" }; Control[] fields = { casePromptChoice, casePromptDirectory, casePromptGoal };
            for (int i = 0; i < fields.Length; i++) {
                inputs.Controls.Add(new Label { Text = labels[i], Dock = DockStyle.Fill, ForeColor = ink, TextAlign = ContentAlignment.MiddleLeft }, 0, i);
                fields[i].AccessibleName = labels[i]; inputs.Controls.Add(fields[i], 1, i);
            }
            panel.Controls.Add(inputs, 0, 1);
            var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false, Margin = new Padding(0) };
            foreach (var button in new[] { generateCasePrompt, copyCasePrompt, saveCasePrompt }) { button.Height = 30; actions.Controls.Add(button); }
            panel.Controls.Add(actions, 0, 2); panel.Controls.Add(casePromptStatus, 0, 3);
            casePromptText.Font = new Font("Microsoft YaHei UI", 10F); casePromptText.BackColor = Color.FromArgb(248, 250, 252); panel.Controls.Add(casePromptText, 0, 4);
            casePromptChoice.SelectedIndexChanged += (sender, args) => { InvalidateCasePrompt(); GenerateCasePrompt(); };
            casePromptGoal.TextChanged += (sender, args) => InvalidateCasePrompt(); casePromptDirectory.TextChanged += (sender, args) => InvalidateCasePrompt();
            url.TextChanged += (sender, args) => InvalidateCasePrompt();
            generateCasePrompt.Click += (sender, args) => GenerateCasePrompt();
            copyCasePrompt.Click += (sender, args) => CopyCasePrompt();
            saveCasePrompt.Click += (sender, args) => {
                using (var dialog = new SaveFileDialog { Filter = "Markdown 提示词|*.md|文本提示词|*.txt", DefaultExt = "md", FileName = "新案例-提示词.md", OverwritePrompt = true })
                    if (dialog.ShowDialog(this) == DialogResult.OK) {
                        try { WriteCasePrompt(dialog.FileName); casePromptStatus.Text = "UTF-8 提示词已保存：" + dialog.FileName; }
                        catch (Exception error) { casePromptStatus.Text = "保存失败：" + error.Message; }
                    }
            };
            tab.Controls.Add(panel); return tab;
        }
        void InvalidateCasePrompt() {
            casePromptRequestId = null; casePromptText.Clear(); copyCasePrompt.Enabled = false; saveCasePrompt.Enabled = false;
            generateCasePrompt.Enabled = ready && !closing && casePromptChoice.SelectedItem is ScenarioChoice;
            casePromptStatus.Text = "输入已变更，请生成 / 更新；目标留空使用默认目标。";
        }
        void GenerateCasePrompt() {
            var choice = casePromptChoice.SelectedItem as ScenarioChoice;
            if (!ready || closing || choice == null) return;
            copyCasePrompt.Enabled = false; saveCasePrompt.Enabled = false; generateCasePrompt.Enabled = false;
            casePromptStatus.Text = "正在生成任务提示词（未执行采集或复现）…";
            try {
                casePromptRequestId = worker.Send("case-prompt", new Dictionary<string, object> { { "templateId", Value(choice.Record, "id") },
                    { "fields", new Dictionary<string, object> { { "url", url.Text.Trim() }, { "goal", casePromptGoal.Text }, { "caseDirectory", casePromptDirectory.Text } } } });
            } catch (Exception error) { casePromptRequestId = null; generateCasePrompt.Enabled = true; casePromptStatus.Text = "模板生成失败：" + error.Message; }
        }
        void WriteCasePrompt(string file) {
            if (!saveCasePrompt.Enabled || String.IsNullOrWhiteSpace(casePromptText.Text)) throw new InvalidOperationException("请先生成当前输入对应的提示词。");
            File.WriteAllText(file, casePromptText.Text, new UTF8Encoding(false));
        }
        void CopyCasePrompt() {
            var text = casePromptText.Text;
            if (!copyCasePrompt.Enabled || String.IsNullOrWhiteSpace(text)) return;
            string error = "剪贴板读回与全文不一致";
            for (int attempt = 0; attempt < 3; attempt++) {
                try {
                    Clipboard.SetText(text, TextDataFormat.UnicodeText);
                    if (Clipboard.GetText(TextDataFormat.UnicodeText) == text) { casePromptStatus.Text = "全文已复制并核验；交给 AI 取证并输出交接文档。"; return; }
                } catch (Exception failure) { error = failure.Message; }
            }
            casePromptStatus.Text = "复制失败：" + error + "；请使用 UTF-8 保存。";
        }
        void ConfigureGrid() {
            grid.AllowUserToAddRows = false; grid.AllowUserToDeleteRows = false; grid.ReadOnly = true; grid.RowHeadersVisible = false;
            grid.SelectionMode = DataGridViewSelectionMode.FullRowSelect; grid.MultiSelect = false; grid.BackgroundColor = Color.White;
            grid.BorderStyle = BorderStyle.None; grid.CellBorderStyle = DataGridViewCellBorderStyle.SingleHorizontal;
            grid.AutoSizeRowsMode = DataGridViewAutoSizeRowsMode.None; grid.RowTemplate.Height = 27;
            grid.EnableHeadersVisualStyles = false; grid.ColumnHeadersDefaultCellStyle.BackColor = Color.FromArgb(232, 239, 244); grid.ColumnHeadersDefaultCellStyle.ForeColor = ink;
            grid.DefaultCellStyle.SelectionBackColor = Color.FromArgb(220, 242, 241); grid.DefaultCellStyle.SelectionForeColor = ink;
            grid.Columns.Add("Time", "时间"); grid.Columns[0].Width = 88; grid.Columns.Add("Category", "类别"); grid.Columns[1].Width = 74;
            grid.Columns.Add("Kind", "事件"); grid.Columns[2].Width = 165; grid.Columns.Add("Target", "Session / Context"); grid.Columns[3].Width = 120;
            grid.Columns.Add("Sequence", "序号"); grid.Columns[4].AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill;
            grid.SelectionChanged += (sender, args) => {
                if (grid.SelectedRows.Count == 0) return;
                var record = grid.SelectedRows[0].Tag as Dictionary<string, object>;
                if (record == null) return;
                details.Text = Value(record, "detail") + (Value(record, "truncated") == "True" ? "\n\n[界面预览已截断，完整事件见日志文件]" : "");
            };
        }

        Dictionary<string, object> VisibleOptions() {
                var options = new Dictionary<string, object> { { "url", url.Text.Trim() }, { "output", ResolveLocalPath(output.Text) }, { "durationSeconds", (int)duration.Value },
                    { "headless", headless.Checked }, { "rawCdp", raw.Checked }, { "transport", activeTransport }, { "recordingControl", recordingControl.Checked }, { "startPaused", startPaused.Checked },
                    { "jsCallValues", jsCallValues.Checked }, { "writeBufferBytes", (long)writeBufferMiB.Value * 1048576 }, { "writeBatchBytes", (long)writeBatchKiB.Value * 1024 },
                    { "objectDepth", (int)objectDepth.Value }, { "objectNodes", (int)objectNodes.Value }, { "objectProperties", (int)objectProperties.Value },
                    { "maxEvents", (int)maxEvents.Value }, { "maxBytes", (long)maxMiB.Value * 1024 * 1024 }, { "maxBodyBytes", (long)bodyMiB.Value * 1024 * 1024 } };
                options["chunkSpool"] = chunkSpool.Checked; options["streamTCP"] = streamTCP.Checked;
                options["vmpProbeFile"] = String.IsNullOrWhiteSpace(vmpProbeFile.Text) ? "" : ResolveLocalPath(vmpProbeFile.Text);
                options["vmpDepth"] = (int)vmpDepth.Value; options["vmpNodes"] = (int)vmpNodes.Value; options["vmpProperties"] = (int)vmpProperties.Value; options["vmpTypedArrayBytes"] = (long)vmpTypedMiB.Value * 1048576; options["vmpMaxActive"] = (int)vmpMaxActive.Value;
                var requiredCapabilities = new List<string>();
                foreach (var mapping in new[] { new[] { "webSocket", "websocket" }, new[] { "indexedDB", "indexeddb" }, new[] { "cookies", "cookie" }, new[] { "storage", "storage" }, new[] { "wasmModules", "wasm.modules" }, new[] { "vmpProbe", "vmp.probe" }, new[] { "envAbsentProbe", "env.absent" }, new[] { "envInterfaceProbe", "env.interfaces" }, new[] { "envHostMutation", "env.mutations" }, new[] { "envInventory", "env.inventory" } }) if (channels[mapping[0]].Checked) requiredCapabilities.Add(mapping[1]);
                if (closureBindings.Checked) requiredCapabilities.Add("jscall.closure"); if (chunkSpool.Checked || streamTCP.Checked) requiredCapabilities.Add("logs.chunk-spool"); if (streamTCP.Checked) requiredCapabilities.Add("logs.stream-tcp"); options["require"] = requiredCapabilities.ToArray();
                if (streamTCP.Checked) { options["streamHost"] = tcpHost.Text.Trim(); options["streamPort"] = (int)tcpPort.Value; if (!String.IsNullOrWhiteSpace(tcpToken.Text)) options["streamTokenFile"] = ResolveLocalPath(tcpToken.Text); }
                if (!String.IsNullOrWhiteSpace(chrome.Text)) options["chromePath"] = ResolveLocalPath(chrome.Text);
                if (activeTransport == "cdp" && !String.IsNullOrWhiteSpace(until.Text)) options["untilExpression"] = until.Text;
                foreach (var channel in channels) options[channel.Key] = channel.Value.Checked;
                options["closureBindings"] = closureBindings.Checked; options["closureScopes"] = (int)closureScopes.Value; options["closureBindingsLimit"] = (int)closureBindingsLimit.Value;
                options["closureDepth"] = (int)closureDepth.Value; options["closureNodes"] = (int)closureNodes.Value; options["closureProperties"] = (int)closureProperties.Value;
                options["envScriptInclude"] = ""; options["envScriptExclude"] = ""; options["envExcludeInternal"] = false; options["envUnknownSource"] = "include"; options["envInventoryProperties"] = (int)envInventoryProperties.Value;
                if (new[] { "envAbsentProbe", "envInterfaceProbe", "envHostMutation", "envInventory" }.Any(key => channels[key].Checked)) {
                    options["envScriptInclude"] = envScriptInclude.Text; options["envScriptExclude"] = envScriptExclude.Text;
                    options["envExcludeInternal"] = envExcludeInternal.Checked; options["envUnknownSource"] = envUnknownSource.SelectedIndex == 1 ? "exclude" : "include";
                    options["envInventoryProperties"] = (int)envInventoryProperties.Value;
                }
                foreach (var item in detailRules) options[item.Key] = "";
                options["detailExcludeInternal"] = false;
                options["detailUnknownSource"] = "include";
                options["nativeKinds"] = "all";
                if (detailFiltering.Checked) {
                    if (!channels["jsCallDetail"].Checked && !channels["nativeDetail"].Checked) throw new InvalidOperationException("调用过滤需要启用逐次 JSCall 或 Native 参数采集。");
                    foreach (var item in detailRules) if (!item.Key.StartsWith("jsValue", StringComparison.Ordinal) || (channels["jsCallDetail"].Checked && jsCallValues.Checked)) options[item.Key] = item.Value.Text;
                    options["detailExcludeInternal"] = detailExcludeInternal.Checked;
                    options["detailUnknownSource"] = detailUnknownSource.SelectedIndex == 0 ? "exclude" : "include";
                    var kinds = nativeKinds.CheckedItems.Cast<ScenarioChoice>().Select(item => Value(item.Record, "id")).ToArray();
                    if (kinds.Length == 0) throw new InvalidOperationException("请至少选择一种 Native 子类。");
                    options["nativeKinds"] = kinds.Length == nativeKinds.Items.Count ? "all" : String.Join(",", kinds);
                }
                if (activePreset != null) options["preset"] = activePreset;
                if (!channels.Values.Any(box => box.Checked) && !raw.Checked) throw new InvalidOperationException("请至少勾选一项采集通道。");
                return options;
        }
        void StartCapture() {
            try {
                var options = CaptureOptions();
                if (!studioAvailable) throw new InvalidOperationException("界面与 C++ 内核版本不一致；请一同更新便携目录后再采集。");
                RequestPreflight(options, true);
            } catch (Exception error) { SetBusy(false); ShowError(error.Message); }
        }
        void StartCaptureNow(Dictionary<string, object> options) {
            try {
                previews.Clear(); RenderEvents(); details.Clear(); summaryText.Clear(); started = DateTime.UtcNow; captureDirectory = null;
                ClearViewer();
                recordingReady = false; recordingPaused = startPaused.Checked; recordingGeneration = -1; recordingMilliseconds = -1; recordingSettled = false; stoppingCapture = false; recordingRequestId = null;
                SetBusy(true); phase.Text = "正在启动 Chrome…"; captureRequestId = worker.Send("start", new Dictionary<string, object> { { "options", options } });
            } catch (Exception error) { SetBusy(false); ShowError(error.Message); }
        }
        void SetBusy(bool value) {
            busy = value; start.Enabled = ready && !value; stop.Enabled = value; load.Enabled = !value;
            if (!value) { recordingReady = false; stoppingCapture = false; recordingRequestId = null; captureRequestId = null; }
            pauseRecording.Enabled = value && recordingReady && recordingRequestId == null;
            recordingControl.Enabled = !value && recordingAvailable; startPaused.Enabled = !value && recordingAvailable && recordingControl.Checked;
            tcpSend.Enabled = ready && tcpAvailable && !value && captureDirectory != null;
            tcpHost.Enabled = tcpPort.Enabled = tcpToken.Enabled = tcpAfterCapture.Enabled = chunkSpool.Enabled = streamTCP.Enabled = !value && tcpAvailable;
            export.Enabled = !value && captureDirectory != null; open.Enabled = captureDirectory != null; refresh.Enabled = !value && captureDirectory != null;
            foreach (var channel in channels.Values) channel.Enabled = !value;
            foreach (Control control in new Control[] { url, chrome, output, duration, maxEvents, maxMiB, bodyMiB, writeBufferMiB, writeBatchKiB, headless, raw, until, exportOutput, transport, objectDepth, objectNodes, objectProperties, preset, presetScriptScope }) control.Enabled = !value;
            ShowPreset();
            generateCasePrompt.Enabled = ready && !closing && casePromptRequestId == null && casePromptChoice.SelectedItem is ScenarioChoice;
            ApplyTransportAvailability();
            UpdateDetailFilters();
            StudioBusy();
            ApplyEditionAvailability();
        }
        void Send(string command, Dictionary<string, object> fields = null) { try { worker.Send(command, fields); } catch (Exception error) { ShowError(error.Message); } }
        void ToggleRecording() {
            if (!busy || !recordingReady || stoppingCapture || recordingRequestId != null) return;
            pauseRecording.Enabled = false; pauseRecording.Text = recordingPaused ? "开启中…" : "暂停中…";
            try { recordingRequestId = worker.Send(recordingPaused ? "resume-recording" : "pause-recording"); }
            catch (Exception error) { recordingRequestId = null; pauseRecording.Enabled = true; pauseRecording.Text = recordingPaused ? "开启采集" : "暂停采集"; ShowError(error.Message); }
        }
        void RecordingMessage(Dictionary<string, object> message) {
            var id = Value(message, "id"); var type = Value(message, "type");
            if (!busy || (id != captureRequestId && id != recordingRequestId)) return;
            if (type == "recording-control-error") { recordingRequestId = null; pauseRecording.Enabled = recordingReady; pauseRecording.Text = recordingPaused ? "开启采集" : "暂停采集"; ShowError(Value(message, "error")); return; }
            long generation = Count(message, "generation"); bool pending = Value(message, "pending") == "True";
            if (generation < recordingGeneration || (generation == recordingGeneration && recordingSettled && pending)) return;
            recordingGeneration = generation; recordingSettled = !pending; recordingPaused = Value(message, "paused") == "True"; recordingReady = Value(message, "ready") == "True" && !stoppingCapture;
            if (!pending) recordingRequestId = null;
            pauseRecording.Enabled = recordingReady && !pending; pauseRecording.Text = pending ? (recordingPaused ? "开启中…" : "暂停中…") : recordingPaused ? "开启采集" : "暂停采集";
            if (message.ContainsKey("recordingActiveMilliseconds")) recordingMilliseconds = Count(message, "recordingActiveMilliseconds");
            var state = Value(message, "state"); phase.Text = state == "paused" ? "已暂停采集 · 浏览器继续运行" : state == "recording" ? "采集中" : state == "pausing" ? "正在暂停并刷盘…" : state == "resuming" ? "正在开启采集…" : state == "finished" ? "采集已停止，正在保存…" : state;
        }
        void Pump() {
            Dictionary<string, object> message;
            for (int i = 0; i < 80 && worker.TryRead(out message); i++) HandleMessage(message);
            if (busy && captureRequestId != null) elapsed.Text = (recordingMilliseconds >= 0 ? TimeSpan.FromMilliseconds(recordingMilliseconds) : DateTime.UtcNow - started).ToString(@"hh\:mm\:ss");
            if (!worker.Alive && !closing && !exitedReported) { exitedReported = true; ready = false; SetBusy(false); ShowError("采集内核已退出；已有证据保留在日志目录。请重新启动软件。"); }
            if (closing && !worker.Alive && !mcpService.Active) { closing = false; Close(); }
        }
        void HandleMessage(Dictionary<string, object> message) {
            var type = Value(message, "type");
            if (StudioMessage(message)) return;
            if (type == "recording-capability") { recordingAvailable = Value(message, "protocol") == "v8trace/recording/1"; recordingControl.Enabled = recordingAvailable && !busy; startPaused.Enabled = recordingAvailable && !busy && recordingControl.Checked; return; }
            if (type == "recording-state" || type == "recording-pending" || type == "recording-control-error") { RecordingMessage(message); return; }
            if (type == "case-prompt" || type == "case-prompt-error") {
                if (Value(message, "id") != casePromptRequestId) return;
                casePromptRequestId = null; generateCasePrompt.Enabled = true;
                if (type == "case-prompt-error") { casePromptText.Clear(); casePromptStatus.Text = "模板生成失败：" + Value(message, "error"); return; }
                casePromptText.Text = Value(message, "text"); copyCasePrompt.Enabled = true; saveCasePrompt.Enabled = true;
                casePromptStatus.Text = "已生成：" + Value(message, "label") + " · 仅任务提示词";
                return;
            }
            if (type == "ready") {
                ApplyEdition(message);
                StudioReady(message);
                recordingAvailable = Value(ObjectValue(message, "recordingControl"), "protocol") == "v8trace/recording/1";
                object kindChoices;
                if (message.TryGetValue("detailFilterKinds", out kindChoices)) foreach (var item in (System.Collections.IEnumerable)kindChoices) {
                    var record = item as Dictionary<string, object>; if (record != null) nativeKinds.Items.Add(new ScenarioChoice { Record = record }, true);
                }
                object choices;
                if (message.TryGetValue("presets", out choices)) foreach (var item in (System.Collections.IEnumerable)choices) {
                    var record = item as Dictionary<string, object>; if (record != null) preset.Items.Add(new ScenarioChoice { Record = record });
                }
                tcpAvailable = Value(message, "runtime") == "cpp";
                tcpStatus.Text = tcpAvailable ? "C++ TCP 传输已就绪" : "内核未声明 TCP 传输能力";
                ready = true; phase.Text = "就绪 · " + editionLabel + " · C++ " + Value(message, "runtimeVersion"); SetBusy(false);
                object promptChoices;
                if (message.TryGetValue("casePromptTemplates", out promptChoices)) foreach (var item in (System.Collections.IEnumerable)promptChoices) {
                    var record = item as Dictionary<string, object>; if (record != null) casePromptChoice.Items.Add(new ScenarioChoice { Record = record });
                }
                if (casePromptChoice.Items.Count > 0) casePromptChoice.SelectedIndex = 0;
            }
            else if (type == "accepted") { captureDirectory = Value(message, "output"); open.Enabled = true; }
            else if (type == "progress") {
                if (message.ContainsKey("recordingActiveMilliseconds")) recordingMilliseconds = Count(message, "recordingActiveMilliseconds");
                var state = Value(message, "phase"); phase.Text = state == "permissions" ? "检查并补齐 Chromium 沙箱权限" : state == "launching" ? "启动 Chrome" : state == "connecting" ? "配置 CDP 通道" : state == "capturing" ? "采集中" : state == "draining" ? "停止并保存证据" : state == "correlating" ? "生成关联证据索引" : state;
                if (state == "paused") phase.Text = "已暂停采集 · 浏览器继续运行"; else if (state == "pausing") phase.Text = "正在暂停并刷盘…"; else if (state == "resuming") phase.Text = "正在开启采集…";
                eventCount.Text = Count(message, "eventCount").ToString("N0"); byteCount.Text = (Count(message, "recordedBytes") / 1048576.0).ToString("N1") + " MiB"; gapCount.Text = Count(message, "criticalGapCount").ToString();
                footer.Text = "界面显示最近 1000 条预览；完整日志保留在目录中 · 预览跳过 " + Count(message, "droppedPreviews") + " 条（非采集丢失）";
            } else if (type == "events") {
                if (Value(message, "replace") == "True") previews.Clear();
                object items; if (message.TryGetValue("events", out items)) foreach (var item in (System.Collections.IEnumerable)items) { var record = item as Dictionary<string, object>; if (record != null) previews.Add(record); }
                if (previews.Count > 1000) previews.RemoveRange(0, previews.Count - 1000); RenderEvents();
                if (Value(message, "replace") == "True") { SetBusy(false); phase.Text = "日志尾部已读取"; }
            } else if (type == "completed" || type == "inspected") {
                captureDirectory = Value(message, "output"); ShowSummary(ObjectValue(message, "summary")); SetBusy(false);
                ClearViewer();
                if (type == "completed" && tcpAfterCapture.Checked && tcpAvailable) SendTcp();
            } else if (type == "transfer-progress") {
                tcpStatus.Text = "传输：" + Value(message, "path") + "\n" + Value(message, "offset") + " / " + Value(message, "bytes") + " 字节";
            } else if (type == "transfer-retry") {
                tcpStatus.Text = "连接中断，准备续传：" + Value(message, "error");
            } else if (type == "transferred") {
                SetBusy(false); tcpStatus.Text = "完整传输并通过 SHA-256 校验"; phase.Text = "TCP 传输完成 · 本地证据保留";
                summaryText.Text = Pretty(ObjectValue(message, "summary"));
            } else if (type == "exported") { SetBusy(false); phase.Text = "案例已导出 · 入口待配置"; summaryText.Text = "导出目录：" + Value(message, "output") + "\n\n" + Pretty(ObjectValue(message, "summary")); }
            else if (type == "stopping") { if (busy) { stoppingCapture = true; phase.Text = "停止请求已发送，正在保存…"; stop.Enabled = false; recordingReady = false; pauseRecording.Enabled = false; } }
            else if (type == "error") { var summary = ObjectValue(message, "summary"); if (summary != null) { captureDirectory = Value(message, "output"); ShowSummary(summary); } SetBusy(false); ShowError(Value(message, "error")); }
            else if (type == "worker-stderr") { summaryText.AppendText("\n内核输出：" + Value(message, "error")); }
        }

        void SendTcp() {
            if (!tcpAvailable || busy || captureDirectory == null) return;
            try {
                var options = new Dictionary<string, object> { { "host", tcpHost.Text.Trim() }, { "port", (int)tcpPort.Value } };
                if (!String.IsNullOrWhiteSpace(tcpToken.Text)) options["tokenFile"] = Path.GetFullPath(tcpToken.Text.Trim());
                worker.Send("tcp-send", new Dictionary<string, object> { { "capture", captureDirectory }, { "options", options } });
                started = DateTime.UtcNow; SetBusy(true); phase.Text = "C++ TCP 传输中"; tcpStatus.Text = "正在校验、压缩并传输；本地证据保留";
            } catch (Exception error) { SetBusy(false); ShowError(error.Message); }
        }
        void ShowSummary(Dictionary<string, object> summary) {
            if (summary == null) return;
            var integrity = Value(summary, "integrityConclusion"); phase.Text = Value(summary, "captureConclusion") == "failed" ? "采集失败 · 证据已保留" : integrity == "partial" ? "已保存 · 存在关键缺口" : "采集完成 · 证据已保存";
            eventCount.Text = Count(summary, "eventCount").ToString("N0"); byteCount.Text = (Count(summary, "recordedBytes") / 1048576.0).ToString("N1") + " MiB"; gapCount.Text = Count(summary, "criticalGapCount").ToString();
            summaryText.Text = "日志目录：" + captureDirectory + "\n\n" + Pretty(summary); footer.Text = "采集结束不等于网站业务成功；查看摘要中的实际能力、精度与缺口。";
        }
        string Pretty(object value) {
            var text = json.Serialize(value); var result = new StringBuilder(); int depth = 0; bool quoted = false, escaped = false;
            foreach (char ch in text) {
                if (quoted) { result.Append(ch); if (escaped) escaped = false; else if (ch == '\\') escaped = true; else if (ch == '"') quoted = false; continue; }
                if (ch == '"') { quoted = true; result.Append(ch); }
                else if (ch == '{' || ch == '[') { result.Append(ch); depth++; result.AppendLine(); result.Append(' ', depth * 2); }
                else if (ch == '}' || ch == ']') { depth--; result.AppendLine(); result.Append(' ', depth * 2); result.Append(ch); }
                else if (ch == ',') { result.Append(ch); result.AppendLine(); result.Append(' ', depth * 2); }
                else if (ch == ':') result.Append(": "); else result.Append(ch);
            }
            return result.ToString();
        }
        void RenderEvents() {
            var previous = grid.SelectedRows.Count > 0 ? grid.SelectedRows[0] : null;
            string selectedSequence = previous != null ? Value(previous.Tag as Dictionary<string, object>, "seq") : "";
            bool following = previous == null || previous.Index == grid.Rows.Count - 1;
            string selected = category.SelectedItem as string ?? "全部"; var search = filter.Text; grid.Rows.Clear();
            foreach (var record in previews) {
                if (selected != "全部" && Value(record, "category") != selected) continue;
                if (search.Length > 0 && Value(record, "detail").IndexOf(search, StringComparison.OrdinalIgnoreCase) < 0) continue;
                DateTime time; string stamp = DateTime.TryParse(Value(record, "observedAt"), out time) ? time.ToLocalTime().ToString("HH:mm:ss.fff") : "";
                string session = Value(record, "sessionId"); if (session.Length > 8) session = session.Substring(0, 8);
                int index = grid.Rows.Add(stamp, Value(record, "category"), Value(record, "kind"), session + " / " + Value(record, "contextId"), Value(record, "seq")); grid.Rows[index].Tag = record;
            }
            if (grid.Rows.Count > 0) {
                var row = following ? grid.Rows[grid.Rows.Count - 1] : grid.Rows.Cast<DataGridViewRow>().FirstOrDefault(item => Value(item.Tag as Dictionary<string, object>, "seq") == selectedSequence) ?? grid.Rows[0];
                grid.ClearSelection(); row.Selected = true; grid.FirstDisplayedScrollingRowIndex = row.Index;
            } else details.Clear();
        }
        void BeginEvidenceCommand(string command, string status, Dictionary<string, object> fields) {
            SetBusy(true); stop.Enabled = false; phase.Text = status;
            try { worker.Send(command, fields); } catch (Exception error) { SetBusy(false); ShowError(error.Message); }
        }
        void ReadEvents() { if (captureDirectory == null) return; string selected = category.SelectedItem as string; if (selected == "全部") { phase.Text = "请选择一个日志类别再读取尾部"; return; } BeginEvidenceCommand("events", "正在读取日志尾部…", new Dictionary<string, object> { { "capture", captureDirectory }, { "category", selected }, { "limit", 200 } }); }
        void LoadCapture() { using (var dialog = new OpenFileDialog { Filter = "采集摘要|summary.json", Title = "选择已有采集包的 summary.json" }) if (dialog.ShowDialog(this) == DialogResult.OK) BeginEvidenceCommand("inspect", "正在载入采集摘要…", new Dictionary<string, object> { { "capture", Path.GetDirectoryName(dialog.FileName) } }); }
        void OpenDirectory() { if (captureDirectory != null && Directory.Exists(captureDirectory)) Process.Start(new ProcessStartInfo(captureDirectory) { UseShellExecute = true }); }
        void ExportCapture() {
            var destination = exportOutput.Text.Trim();
            if (destination.Length == 0) {
                using (var dialog = new FolderBrowserDialog { Description = "选择案例导出根目录，将自动创建新的导出子目录" }) {
                    if (dialog.ShowDialog(this) != DialogResult.OK) return;
                    destination = Path.Combine(dialog.SelectedPath, "trace-import-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff"));
                }
            }
            BeginEvidenceCommand("export", "正在核验并导出资源…", new Dictionary<string, object> { { "capture", captureDirectory }, { "output", ResolveLocalPath(destination) } });
        }
        static string ApplicationRoot { get { return Path.GetDirectoryName(typeof(TraceWindow).Assembly.Location); } }
        static string ResolveLocalPath(string value) {
            value = value.Trim();
            if (value.Length == 0) throw new ArgumentException("请填写目录或文件路径。");
            if (Path.IsPathRooted(value)) return Path.GetFullPath(value);
            return Path.GetFullPath(Path.Combine(ApplicationRoot, value));
        }
        static string DisplayLocalPath(string value) {
            var absolute = ResolveLocalPath(value); var prefix = ApplicationRoot.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            return absolute.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) ? @".\" + absolute.Substring(prefix.Length) : absolute;
        }
        void NewOutput() { output.Text = Path.Combine(@".\logs", "capture-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff")); }
        static string FindChrome() {
            var bundled = FindOpcodeChrome(); if (bundled != null) return bundled;
            var configured = Environment.GetEnvironmentVariable("CHROME_PATH");
            return !String.IsNullOrEmpty(configured) && File.Exists(ResolveLocalPath(configured)) ? DisplayLocalPath(configured) : @".\chromium\chrome.exe";
        }
        static string FindOpcodeChrome() {
            var found = new string[] { @".\chromium\chrome.exe", Environment.GetEnvironmentVariable("V8_TRACE_CHROMIUM_PATH") }.FirstOrDefault(file => !String.IsNullOrEmpty(file) && File.Exists(ResolveLocalPath(file)) && File.Exists(Path.Combine(Path.GetDirectoryName(ResolveLocalPath(file)), "v8trace-opcode.json")));
            return found == null ? null : DisplayLocalPath(found);
        }
        void ShowError(string error) { phase.Text = "操作错误 · 查看摘要"; summaryText.AppendText("\n错误：" + error + "\n"); footer.Text = error; }
        void ClosingWindow(object sender, FormClosingEventArgs args) {
            if (!worker.Alive && !mcpService.Active) return;
            if (busy && !closing && MessageBox.Show(this, "任务正在运行。停止并保留已有证据后退出？", "退出 V8 Trace", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) { args.Cancel = true; return; }
            args.Cancel = true;
            if (!closing) { closing = true; phase.Text = "正在保存并退出…"; StopMcp(); if (worker.Alive) Send("shutdown"); }
        }
    }

    static class Program {
        [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
        [STAThread] static void Main() {
            SetProcessDPIAware(); Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            try { Application.Run(new TraceWindow()); }
            catch (Exception error) { MessageBox.Show(error.Message, "V8 Trace 启动错误", MessageBoxButtons.OK, MessageBoxIcon.Error); }
        }
    }
}
