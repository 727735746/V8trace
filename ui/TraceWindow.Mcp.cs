using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace V8Trace {
    public sealed partial class TraceWindow {
        readonly McpService mcpService = new McpService();
        readonly TextBox mcpSource = new TextBox { Name = "McpSourceRoot", Text = @".\logs" };
        readonly TextBox mcpIndex = new TextBox { Name = "McpIndexRoot", Text = @".\mcp\index" };
        readonly TextBox mcpExports = new TextBox { Name = "McpExportRoot", Text = @".\mcp\exports" };
        readonly TextBox mcpToken = new TextBox { Name = "McpTokenFile", Text = @".\mcp\token.txt" };
        readonly NumericUpDown mcpPort = Number("McpPort", 17881, 1024, 65535);
        readonly Button mcpStart = new Button { Name = "McpStartButton", Text = "启动 MCP", AutoSize = true };
        readonly Button mcpStop = new Button { Name = "McpStopButton", Text = "停止服务", AutoSize = true, Enabled = false };
        readonly Label mcpStatus = new Label { Name = "McpStatus", Dock = DockStyle.Fill, ForeColor = Color.FromArgb(0, 117, 120), Text = "未启动 · 不自动监听端口，采集与搜索使用独立 C++ 进程。" };
        readonly RichTextBox mcpConfiguration = new RichTextBox { Name = "McpClientConfiguration", Dock = DockStyle.Fill, ReadOnly = true, WordWrap = false, Font = new Font("Consolas", 9F) };
        bool mcpReady;

        Button McpButton(string name, string label, Action action) {
            var button = new Button { Name = name, Text = label, AutoSize = true };
            button.Click += (sender, args) => { try { action(); } catch (Exception error) { mcpStatus.Text = "操作失败：" + error.Message; } }; return button;
        }
        TabPage BuildMcpTab() {
            var tab = new TabPage("MCP 搜索 / 分析") { Name = "McpTab", BackColor = Color.White };
            var panel = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(8), ColumnCount = 1, RowCount = 5 };
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 40)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 140)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 32)); panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 32)); panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            panel.Controls.Add(new Label { Dock = DockStyle.Fill, ForeColor = ink, Text = "C++ 原生大日志搜索 + JSCall / Opcode / Native / HTTP 证据查询。\n相对路径按软件目录解析；只读已有日志，不执行日志内容。" }, 0, 0);
            var fields = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 5, Margin = new Padding(0) };
            fields.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 84)); fields.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); fields.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 72));
            var labels = new[] { "日志根目录", "索引目录", "导出目录", "令牌文件", "本地端口" }; Control[] inputs = { mcpSource, mcpIndex, mcpExports, mcpToken, mcpPort };
            for (int n = 0; n < inputs.Length; ++n) { fields.RowStyles.Add(new RowStyle(SizeType.Absolute, 28)); fields.Controls.Add(new Label { Text = labels[n], TextAlign = ContentAlignment.MiddleLeft, Dock = DockStyle.Fill }, 0, n); inputs[n].Dock = DockStyle.Fill; inputs[n].AccessibleName = labels[n]; fields.Controls.Add(inputs[n], 1, n); }
            fields.Controls.Add(McpButton("McpBrowseSource", "选择…", () => { using (var dialog = new FolderBrowserDialog { Description = "选择允许 MCP 读取的已有日志根目录" }) if (dialog.ShowDialog(this) == DialogResult.OK) mcpSource.Text = DisplayLocalPath(dialog.SelectedPath); }), 2, 0);
            fields.Controls.Add(McpButton("McpCurrentCapture", "当前包", () => { if (captureDirectory == null) throw new InvalidOperationException("请先载入或保存一个采集包。"); mcpSource.Text = DisplayLocalPath(captureDirectory); }), 2, 1);
            fields.Controls.Add(McpButton("McpTokenButton", "生成令牌", () => { McpService.Token(ResolveLocalPath(mcpToken.Text)); mcpStatus.Text = "已生成或复用当前用户令牌；现有令牌保持原值。"; }), 2, 3);
            mcpPort.ThousandsSeparator = false; panel.Controls.Add(fields, 0, 1);
            var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false, Margin = new Padding(0) };
            mcpStart.Click += (sender, args) => { try { StartMcp(); } catch (Exception error) { mcpStatus.Text = "启动失败：" + error.Message; } }; mcpStop.Click += (sender, args) => StopMcp();
            actions.Controls.Add(mcpStart); actions.Controls.Add(mcpStop);
            actions.Controls.Add(McpButton("McpCopyHttp", "复制 HTTP 配置", () => CopyMcpConfiguration("http")));
            actions.Controls.Add(McpButton("McpCopyStdio", "复制 stdio 配置", () => CopyMcpConfiguration("stdio")));
            actions.Controls.Add(McpButton("McpManualButton", "操作说明", () => { var file = Path.Combine(ApplicationRoot, "MCP搜索分析说明.md"); if (!File.Exists(file)) throw new FileNotFoundException("操作说明文件缺失。"); System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(file) { UseShellExecute = true }); }));
            panel.Controls.Add(actions, 0, 2); panel.Controls.Add(mcpStatus, 0, 3); panel.Controls.Add(mcpConfiguration, 0, 4); tab.Controls.Add(panel);
            mcpConfiguration.Text = "先选择日志根目录，再启动本地 MCP。\n\n客户端工具：\nopen_file → get_file_status → search_lines / read_lines / export_lines → close_file\n\nv8trace_open_capture → v8trace_query → v8trace_read_evidence\n\n复制配置只生成配置文本，不自动修改全局 MCP 设置。\nstdio 客户端自启独立进程，不需要先启动此 HTTP 服务。"; return tab;
        }
        object McpClientConfiguration(string transport) {
            var config = new Dictionary<string, object>();
            if (transport == "http") { config["url"] = "http://127.0.0.1:" + mcpPort.Value + "/mcp"; config["headers"] = new Dictionary<string, object> { { "Authorization", "Bearer " + McpService.Token(ResolveLocalPath(mcpToken.Text)) } }; }
            else { config["command"] = Path.Combine(ApplicationRoot, "runtime", "v8trace-runtime.exe"); config["args"] = new[] { "mcp", "--transport", "stdio", "--source-root", ResolveLocalPath(mcpSource.Text), "--index-dir", ResolveLocalPath(mcpIndex.Text) + "-stdio", "--export-root", ResolveLocalPath(mcpExports.Text) }; }
            return new Dictionary<string, object> { { "mcpServers", new Dictionary<string, object> { { "v8trace", config } } } };
        }
        void CopyMcpConfiguration(string transport) { var text = Pretty(McpClientConfiguration(transport)); mcpConfiguration.Text = text; Clipboard.SetText(text); mcpStatus.Text = "已复制 " + transport + " 配置。配置按当前软件位置生成，移动目录后重新复制。"; }
        void StartMcp() { if (logsDeleting) return; mcpReady = false; mcpService.Start(ApplicationRoot, ResolveLocalPath(mcpSource.Text), ResolveLocalPath(mcpIndex.Text), ResolveLocalPath(mcpExports.Text), ResolveLocalPath(mcpToken.Text), (int)mcpPort.Value); mcpStatus.Text = "正在启动 C++ MCP，等待真实监听回执…"; McpControls(); }
        void StopMcp() { if (!mcpService.Active) return; mcpReady = false; mcpService.RequestStop(); mcpStatus.Text = "正在停止自有 MCP 服务并取消待完成查询…"; McpControls(); }
        void McpControls() { bool active = mcpService.Active; mcpStart.Enabled = !active && !closing && !logsDeleting; mcpStop.Enabled = active && !closing; mcpStop.AccessibleDescription = mcpReady ? "结束当前 C++ MCP 服务" : "取消启动或停止当前 C++ MCP 服务"; foreach (Control input in new Control[] { mcpSource, mcpIndex, mcpExports, mcpToken, mcpPort }) input.Enabled = !active && !logsDeleting; foreach (var name in new[] { "McpBrowseSource", "McpCurrentCapture", "McpTokenButton" }) foreach (Control control in Controls.Find(name, true)) control.Enabled = !active && !logsDeleting; }
        void McpTick() {
            mcpService.Poll(); string line;
            for (int n = 0; n < 64 && mcpService.TryRead(out line); ++n) {
                try { var message = json.Deserialize<Dictionary<string, object>>(line); string type = Value(message, "type");
                    if (type == "mcp-ready") { mcpReady = true; mcpStatus.Text = "已监听 " + Value(message, "endpoint") + " · " + Value(message, "tools") + " 个 C++ 工具"; }
                    else if (type == "mcp-stopped") { mcpReady = false; mcpStatus.Text = Value(message, "forced") == "True" ? "服务超时后已结束自有进程；缓存下次核验，原始日志保留。" : "MCP 服务已停止；原始日志与可复用索引保留。"; }
                    else if (type == "mcp-exited") { mcpReady = false; mcpStatus.Text = "MCP 进程退出，code=" + Value(message, "code") + "。请核对端口及下方诊断。"; }
                    else { mcpConfiguration.AppendText("\n" + line); mcpStatus.Text = "MCP 诊断：" + Value(message, "error"); }
                } catch { mcpConfiguration.AppendText("\n" + line); }
            }
            McpControls();
        }
    }
}
