using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace V8Trace {
    public static class CyberPalette {
        [DllImport("user32.dll", EntryPoint = "SystemParametersInfoW")] static extern bool SystemParametersInfo(uint action, uint parameter, out int value, uint flags);
        public static bool SystemAllowsMotion { get { int enabled; return !SystemInformation.HighContrast && SystemParametersInfo(0x1042, 0, out enabled, 0) && enabled != 0; } }
        public static ThemeDefinition Theme = ThemePreferences.Themes[0];
        public static Color Primary = Theme.Primary, Secondary = Theme.Secondary;
        public static Color Background { get { return Theme.Background; } }
        public static Color Surface { get { return Theme.Surface; } }
        public static Color Input { get { return Theme.Input; } }
        public static Color Border { get { return Theme.Border; } }
        public static Color Text { get { return Theme.Text; } }
        public static Color Muted { get { return Theme.Muted; } }
        public static Color Cyan { get { return Readable(Primary, Surface); } }
        public static Color Pink { get { return Readable(Secondary, Surface); } }
        public static Color Danger { get { return Theme.Dark ? Color.FromArgb(255, 135, 161) : Color.FromArgb(174, 29, 61); } }
        public static Color Amber { get { return Theme.Dark ? Color.FromArgb(255, 204, 102) : Color.FromArgb(137, 81, 0); } }
        public static Color Selection { get { return Blend(Surface, Primary, Theme.Dark ? 0.19 : 0.12); } }
        public static Color Blend(Color left, Color right, double part) { return Color.FromArgb((int)(left.R + (right.R - left.R) * part), (int)(left.G + (right.G - left.G) * part), (int)(left.B + (right.B - left.B) * part)); }
        static double Linear(int component) { double value = component / 255.0; return value <= 0.04045 ? value / 12.92 : Math.Pow((value + 0.055) / 1.055, 2.4); }
        static double Luminance(Color color) { return 0.2126 * Linear(color.R) + 0.7152 * Linear(color.G) + 0.0722 * Linear(color.B); }
        public static double Contrast(Color foreground, Color background) { var left = Luminance(foreground); var right = Luminance(background); return (Math.Max(left, right) + 0.05) / (Math.Min(left, right) + 0.05); }
        public static Color ButtonText(Color background) { return Contrast(Color.Black, background) >= Contrast(Color.White, background) ? Color.Black : Color.White; }
        public static Color Readable(Color color, Color background) {
            if (Contrast(color, background) >= 4.5) return color;
            var target = ButtonText(background);
            for (int step = 1; step <= 20; ++step) { var result = Blend(color, target, step / 20.0); if (Contrast(result, background) >= 4.5) return result; }
            return target;
        }
        public static bool Phantom { get { return Theme.Id.StartsWith("phantom", StringComparison.Ordinal); } }
        public static void DrawSpectrum(Graphics graphics, Rectangle bounds) {
            if (bounds.Width <= 0 || bounds.Height <= 0) return;
            using (var brush = new LinearGradientBrush(bounds, Primary, Secondary, 0F)) {
                if (Phantom) brush.InterpolationColors = new ColorBlend { Colors = new[] { Primary, Color.FromArgb(74, 98, 255), Color.FromArgb(151, 78, 255), Secondary, Color.FromArgb(179, 134, 255) }, Positions = new[] { 0F, 0.28F, 0.55F, 0.84F, 1F } };
                graphics.FillRectangle(brush, bounds);
            }
        }
        static void DrawTab(object sender, DrawItemEventArgs args) {
            var tabs = (TabControl)sender; if (args.Index < 0 || args.Index >= tabs.TabCount) return;
            var active = args.Index == tabs.SelectedIndex;
            using (var brush = new SolidBrush(active ? Input : Background)) args.Graphics.FillRectangle(brush, args.Bounds);
            TextRenderer.DrawText(args.Graphics, tabs.TabPages[args.Index].Text, tabs.Font, args.Bounds, active ? Cyan : Muted, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
            if (active) DrawSpectrum(args.Graphics, new Rectangle(args.Bounds.Left + 5, args.Bounds.Bottom - 2, Math.Max(1, args.Bounds.Width - 10), 2));
        }
        static void DrawChoice(object sender, DrawItemEventArgs args) {
            var choice = (ComboBox)sender; var selected = (args.State & DrawItemState.Selected) != 0;
            using (var brush = new SolidBrush(selected ? Selection : Input)) args.Graphics.FillRectangle(brush, args.Bounds);
            var text = args.Index >= 0 && args.Index < choice.Items.Count ? choice.GetItemText(choice.Items[args.Index]) : choice.Text;
            TextRenderer.DrawText(args.Graphics, text, choice.Font, new Rectangle(args.Bounds.Left + 4, args.Bounds.Top, Math.Max(0, args.Bounds.Width - 8), args.Bounds.Height), Readable(Text, selected ? Selection : Input), TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
            args.DrawFocusRectangle();
        }
        static void DrawDisabledButton(object sender, PaintEventArgs args) {
            var button = (Button)sender; if (button.Enabled || button is CyberNavigationButton) return;
            using (var brush = new SolidBrush(Input)) args.Graphics.FillRectangle(brush, button.ClientRectangle);
            using (var pen = new Pen(Border)) args.Graphics.DrawRectangle(pen, 0, 0, Math.Max(0, button.Width - 1), Math.Max(0, button.Height - 1));
            TextRenderer.DrawText(args.Graphics, button.Text, button.Font, new Rectangle(3, 1, Math.Max(0, button.Width - 6), Math.Max(0, button.Height - 2)), Muted, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
        }
        public static void Apply(Control control) {
            control.BackColor = Surface; control.ForeColor = Text;
            var label = control as Label;
            if (label != null) { label.BackColor = Color.Transparent; label.ForeColor = label.Font.Size >= 15 ? Cyan : Text; }
            var button = control as Button;
            if (button != null) {
                button.FlatStyle = FlatStyle.Flat; button.FlatAppearance.BorderSize = 1; button.FlatAppearance.BorderColor = Border;
                button.FlatAppearance.MouseOverBackColor = Blend(Input, Primary, 0.10); button.FlatAppearance.MouseDownBackColor = Selection;
                button.BackColor = Input; button.ForeColor = Text; button.Cursor = Cursors.Hand;
                button.Paint -= DrawDisabledButton; button.Paint += DrawDisabledButton;
            }
            if (control is TextBoxBase || control is ComboBox || control is ListBox || control is NumericUpDown) { control.BackColor = Input; control.ForeColor = Text; }
            var edit = control as TextBoxBase; if (edit != null) edit.BorderStyle = BorderStyle.FixedSingle;
            var choice = control as ComboBox; if (choice != null && choice.DropDownStyle == ComboBoxStyle.DropDownList) { choice.FlatStyle = FlatStyle.Flat; choice.DrawMode = DrawMode.OwnerDrawFixed; choice.ItemHeight = Math.Max(20, choice.Font.Height + 4); choice.DrawItem -= DrawChoice; choice.DrawItem += DrawChoice; }
            var rich = control as RichTextBox; if (rich != null) rich.BorderStyle = BorderStyle.None;
            var check = control as CheckBox; if (check != null) { check.FlatStyle = FlatStyle.Flat; check.BackColor = Color.Transparent; }
            var grid = control as DataGridView;
            if (grid != null) {
                grid.EnableHeadersVisualStyles = false; grid.BackgroundColor = Surface; grid.BorderStyle = BorderStyle.None;
                grid.GridColor = Border; grid.CellBorderStyle = DataGridViewCellBorderStyle.SingleHorizontal;
                grid.ColumnHeadersBorderStyle = DataGridViewHeaderBorderStyle.None; grid.ColumnHeadersHeight = 36;
                grid.ColumnHeadersDefaultCellStyle.BackColor = Input; grid.ColumnHeadersDefaultCellStyle.ForeColor = Cyan;
                grid.ColumnHeadersDefaultCellStyle.Font = control.Font;
                grid.DefaultCellStyle.BackColor = Surface; grid.DefaultCellStyle.ForeColor = Text;
                grid.DefaultCellStyle.SelectionBackColor = Selection; grid.DefaultCellStyle.SelectionForeColor = Readable(Text, Selection);
                grid.AlternatingRowsDefaultCellStyle.BackColor = Blend(Surface, Background, 0.4); grid.RowTemplate.Height = 30;
                foreach (DataGridViewRow row in grid.Rows) { row.DefaultCellStyle.ForeColor = Color.Empty; row.DefaultCellStyle.BackColor = Color.Empty; }
            }
            var tabs = control as TabControl;
            if (tabs != null && !(tabs is CyberWorkspaceTabs)) {
                tabs.DrawMode = TabDrawMode.OwnerDrawFixed; tabs.SizeMode = TabSizeMode.Normal; tabs.Padding = new Point(14, 7);
                tabs.DrawItem -= DrawTab; tabs.DrawItem += DrawTab;
            }
            foreach (Control child in control.Controls) Apply(child);
        }
    }
    public sealed class CyberHeaderPanel : TableLayoutPanel {
        public double AnimationPhase;
        public bool Animate;
        public CyberHeaderPanel() { DoubleBuffered = true; }
        void DrawSmoke(Graphics graphics) {
            var phase = AnimationPhase * Math.PI * 2;
            for (int layer = 0; layer < 8; ++layer) {
                var drift = (AnimationPhase * 0.38 + layer / 8.0) % 1.0;
                var width = 220 + layer % 3 * 65;
                var x = (float)(drift * (Width + width) - width);
                var y = (float)(Height * 0.25 + Math.Sin(phase * 0.7 + layer * 1.4) * 15);
                using (var cloud = new GraphicsPath()) {
                    cloud.AddEllipse(x, y, width, 70 + layer % 3 * 14);
                    using (var brush = new PathGradientBrush(cloud)) {
                        brush.CenterColor = Color.FromArgb(42 + layer % 3 * 12, layer % 2 == 0 ? CyberPalette.Primary : CyberPalette.Secondary);
                        brush.SurroundColors = new[] { Color.Transparent };
                        graphics.FillPath(brush, cloud);
                    }
                }
            }
            for (int layer = 0; layer < 2; ++layer) {
                float wave = (float)Math.Sin(phase + layer * 2.1) * 18, middle = Height * 0.62F;
                using (var ribbon = new GraphicsPath()) {
                    ribbon.AddBezier(-40, middle + wave, Width * 0.3F, middle - 32 - wave, Width * 0.7F, middle + 28 + wave, Width + 40, middle - wave);
                    ribbon.AddBezier(Width + 40, middle + 24 - wave, Width * 0.7F, middle + 50 + wave, Width * 0.3F, middle - 6 - wave, -40, middle + 24 + wave);
                    ribbon.CloseFigure();
                    using (var brush = new PathGradientBrush(ribbon)) {
                        brush.CenterColor = Color.FromArgb(62, layer == 0 ? CyberPalette.Primary : CyberPalette.Secondary);
                        brush.SurroundColors = new[] { Color.Transparent };
                        graphics.FillPath(brush, ribbon);
                    }
                }
            }
        }
        protected override void OnPaint(PaintEventArgs args) {
            base.OnPaint(args);
            CyberPalette.DrawSpectrum(args.Graphics, new Rectangle(0, Height - (CyberPalette.Theme.Multicolor ? 3 : 2), Width, CyberPalette.Theme.Multicolor ? 3 : 2));
            if (CyberPalette.Theme.Multicolor) {
                using (var pen = new Pen(CyberPalette.Blend(CyberPalette.Surface, CyberPalette.Secondary, 0.35))) {
                    for (int x = Width - 220; x < Width - 10; x += 14) args.Graphics.DrawLine(pen, x, Height - 16, x + 6, Height - 22);
                }
            }
            if (Animate && CyberPalette.Phantom) {
                args.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
                if (CyberPalette.Theme.Id == "phantom-smoke") DrawSmoke(args.Graphics);
                var glow = new Rectangle(0, Math.Max(0, Height - 20), Width, 20);
                using (var brush = new LinearGradientBrush(glow, Color.Transparent, Color.FromArgb(72, CyberPalette.Primary), 90F)) args.Graphics.FillRectangle(brush, glow);
                for (int trail = 0; trail < 2; ++trail) {
                    var position = (AnimationPhase * (trail == 0 ? 1 : -0.7) + trail * 0.5) % 1.0;
                    if (position < 0) position += 1;
                    var region = new RectangleF((float)(position * (Width + 240) - 240), Height - 12 + trail * 6, 240, trail == 0 ? 4 : 3);
                    using (var brush = new LinearGradientBrush(region, Color.Transparent, CyberPalette.Primary, 0F)) {
                        brush.InterpolationColors = new ColorBlend { Colors = new[] { Color.Transparent, Color.FromArgb(200, CyberPalette.Primary), Color.White, Color.FromArgb(200, CyberPalette.Secondary), Color.Transparent }, Positions = new[] { 0F, 0.4F, 0.5F, 0.62F, 1F } };
                        args.Graphics.FillRectangle(brush, region);
                    }
                }
                var count = CyberPalette.Theme.Multicolor ? 24 : 14;
                for (int i = 0; i < count; ++i) {
                    var x = (float)(((AnimationPhase * 0.7 + i / (double)count) % 1.0) * Width);
                    var y = Height - 15 + (float)Math.Sin(AnimationPhase * Math.PI * 2 + i * 1.8) * 4;
                    var color = i % 2 == 0 ? CyberPalette.Primary : CyberPalette.Secondary;
                    using (var brush = new SolidBrush(Color.FromArgb(36, color))) args.Graphics.FillEllipse(brush, x - 3, y - 3, 9, 9);
                    using (var brush = new SolidBrush(Color.FromArgb(165 + i % 3 * 40, color))) args.Graphics.FillEllipse(brush, x, y, 3, 3);
                }
            }
        }
    }
    public sealed class ThemeMotionCheckBox : CheckBox {
        string stateText = "已开启";
        public string StateText { get { return stateText; } set { if (stateText != value) { stateText = value; Invalidate(); } } }
        public ThemeMotionCheckBox() {
            AutoSize = false; AutoCheck = true; ThreeState = false; Cursor = Cursors.Hand;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        }
        protected override void OnCheckedChanged(EventArgs args) { base.OnCheckedChanged(args); Invalidate(); }
        protected override void OnPaint(PaintEventArgs args) {
            args.Graphics.Clear(CyberPalette.Surface);
            args.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            int edge = Math.Max(17, Font.Height), top = (Height - edge) / 2;
            var box = new Rectangle(2, top, edge, edge);
            using (var brush = new SolidBrush(Checked ? CyberPalette.Primary : CyberPalette.Input)) args.Graphics.FillRectangle(brush, box);
            using (var pen = new Pen(Checked ? CyberPalette.Primary : CyberPalette.Muted, 1.5F)) args.Graphics.DrawRectangle(pen, box);
            if (Checked) using (var pen = new Pen(CyberPalette.ButtonText(CyberPalette.Primary), 2.2F)) {
                pen.StartCap = pen.EndCap = LineCap.Round;
                args.Graphics.DrawLines(pen, new[] { new PointF(box.Left + edge * 0.23F, box.Top + edge * 0.52F), new PointF(box.Left + edge * 0.44F, box.Top + edge * 0.73F), new PointF(box.Left + edge * 0.79F, box.Top + edge * 0.27F) });
            }
            int statusWidth = TextRenderer.MeasureText(stateText, Font).Width + 4, textLeft = edge + 10;
            TextRenderer.DrawText(args.Graphics, Text, Font, new Rectangle(textLeft, 0, Math.Max(0, Width - textLeft - statusWidth - 6), Height), CyberPalette.Text, TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
            TextRenderer.DrawText(args.Graphics, stateText, Font, new Rectangle(Width - statusWidth - 2, 0, statusWidth, Height), Checked ? CyberPalette.Cyan : CyberPalette.Muted, TextFormatFlags.Right | TextFormatFlags.VerticalCenter);
            if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(args.Graphics, new Rectangle(0, 0, Math.Max(0, Width - 1), Math.Max(0, Height - 1)), CyberPalette.Cyan, CyberPalette.Surface);
        }
    }
    public sealed class CyberWorkspaceTabs : TabControl {
        public CyberWorkspaceTabs() { Appearance = TabAppearance.FlatButtons; ItemSize = new Size(1, 1); SizeMode = TabSizeMode.Fixed; Multiline = true; TabStop = false; }
        public override Rectangle DisplayRectangle { get { return new Rectangle(0, 0, ClientSize.Width, ClientSize.Height); } }
        protected override void WndProc(ref Message message) {
            if (message.Msg == 0x1328 && !DesignMode) { message.Result = new IntPtr(1); return; }
            base.WndProc(ref message);
        }
    }
    public sealed class CyberNavigationButton : Button {
        public bool Selected;
        protected override void OnPaint(PaintEventArgs args) {
            var color = Selected ? CyberPalette.Selection : CyberPalette.Background;
            args.Graphics.Clear(color);
            if (Selected) {
                CyberPalette.DrawSpectrum(args.Graphics, new Rectangle(0, 5, 3, Height - 10));
                using (var pen = new Pen(CyberPalette.Blend(CyberPalette.Border, CyberPalette.Primary, 0.35))) args.Graphics.DrawRectangle(pen, 0, 1, Width - 1, Height - 3);
                if (CyberPalette.Theme.Multicolor) CyberPalette.DrawSpectrum(args.Graphics, new Rectangle(1, Height - 3, Width - 2, 2));
            }
            TextRenderer.DrawText(args.Graphics, Text, Font, new Rectangle(14, 0, Width - 20, Height), Enabled ? (Selected ? CyberPalette.Readable(CyberPalette.Primary, color) : CyberPalette.Muted) : CyberPalette.Muted,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
            if (Focused) ControlPaint.DrawFocusRectangle(args.Graphics, new Rectangle(5, 5, Width - 10, Height - 10), CyberPalette.Cyan, color);
        }
        protected override void OnMouseEnter(EventArgs args) { base.OnMouseEnter(args); ForeColor = CyberPalette.Cyan; Invalidate(); }
        protected override void OnMouseLeave(EventArgs args) { base.OnMouseLeave(args); Invalidate(); }
    }
    public sealed partial class TraceWindow {
        readonly CyberWorkspaceTabs workspaces = new CyberWorkspaceTabs { Name = "WorkspaceTabs", Dock = DockStyle.Fill };
        readonly FlowLayoutPanel cyberNavigation = new FlowLayoutPanel { Name = "WorkspaceNavigation", Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true, Margin = new Padding(0) };
        readonly Dictionary<TabPage, CyberNavigationButton> workspaceNavigation = new Dictionary<TabPage, CyberNavigationButton>();
        readonly ComboBox captureSection = new ComboBox { Name = "CaptureSectionSelector", Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
        readonly Label editionBadge = new Label { Name = "EditionBadge", AutoSize = true, Padding = new Padding(10, 5, 10, 5), Text = "ENGINE / CONNECTING", Font = new Font("Consolas", 9F, FontStyle.Bold) };
        TabPage captureWorkspace, logManagementWorkspace;
        bool finishingCyber, changingTheme, cyberEditionKnown;
        bool navigationLayoutPending;
        ThemePreferences themePreferences;
        CyberHeaderPanel cyberHeader;
        readonly System.Windows.Forms.Timer themeAnimation = new System.Windows.Forms.Timer { Interval = 50 };
        readonly System.Diagnostics.Stopwatch themeAnimationClock = new System.Diagnostics.Stopwatch();
        readonly ThemeMotionCheckBox themeMotion = new ThemeMotionCheckBox { Name = "ThemeMotionToggle", Text = "动态光效", Dock = DockStyle.Fill, Margin = new Padding(0) };
        readonly ComboBox themeSelector = new ComboBox { Name = "ThemeSelector", Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList, DropDownWidth = 255 };
        readonly Button primaryColor = new Button { Name = "ThemeAccentButton", Text = "主色", Dock = DockStyle.Fill };
        readonly Button secondaryColor = new Button { Name = "ThemeSecondaryButton", Text = "辅色", Dock = DockStyle.Fill };
        readonly Label themeStatus = new Label { Name = "ThemeStatus", Dock = DockStyle.Fill, Text = "外观仅保存在本机，不影响采集。", Font = new Font("Microsoft YaHei UI", 8F), Margin = new Padding(0), AutoEllipsis = true };
        readonly Label themeCaption = new Label { Name = "ThemeCaption", Dock = DockStyle.Fill, Font = new Font("Consolas", 8.5F), TextAlign = ContentAlignment.MiddleLeft, AutoEllipsis = true };

        void BuildCyberWindow(Control captureBody) {
            var root = new TableLayoutPanel { Name = "CyberRoot", Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3, Padding = new Padding(18, 0, 18, 0) };
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 96)); root.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); root.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));
            var header = new CyberHeaderPanel { Name = "CyberHeader", Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Padding = new Padding(8, 8, 0, 18) };
            cyberHeader = header;
            header.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 55)); header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 45));
            try { Icon = Icon.ExtractAssociatedIcon(typeof(TraceWindow).Assembly.Location); } catch (Exception) { }
            var brand = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2, ColumnCount = 1, Margin = new Padding(0) };
            brand.RowStyles.Add(new RowStyle(SizeType.Absolute, 38)); brand.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            brand.Controls.Add(new Label { Name = "BrandTitle", Text = "V8 TRACE / 浏览器采集", Dock = DockStyle.Fill, Font = new Font("Microsoft YaHei UI", 18F, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft, AutoEllipsis = true }, 0, 0);
            brand.Controls.Add(themeCaption, 0, 1);
            var status = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2, ColumnCount = 1 };
            status.RowStyles.Add(new RowStyle(SizeType.Absolute, 30)); status.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            editionBadge.Anchor = AnchorStyles.Right | AnchorStyles.Top; status.Controls.Add(editionBadge, 0, 0);
            phase.Dock = DockStyle.Fill; phase.AutoEllipsis = true; phase.TextAlign = ContentAlignment.MiddleRight; status.Controls.Add(phase, 0, 1);
            header.Controls.Add(brand, 0, 0); header.Controls.Add(status, 1, 0);
            root.Controls.Add(header, 0, 0);
            var body = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Margin = new Padding(0) };
            body.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 188)); body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            var navigation = new TableLayoutPanel { Name = "CyberSidebar", Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3, Padding = new Padding(0, 12, 12, 6), Margin = new Padding(0) };
            navigation.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            navigation.RowStyles.Add(new RowStyle(SizeType.Absolute, 30)); navigation.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); navigation.RowStyles.Add(new RowStyle(SizeType.Absolute, 202));
            navigation.Controls.Add(new Label { Text = "WORKSPACE / 工作区", Dock = DockStyle.Fill, Font = new Font("Consolas", 8F), Margin = new Padding(0) }, 0, 0);
            navigation.Controls.Add(cyberNavigation, 0, 1);
            cyberNavigation.ClientSizeChanged += delegate { QueueCyberNavigationLayout(); };
            cyberNavigation.Layout += delegate { QueueCyberNavigationLayout(); };
            navigation.Controls.Add(BuildAppearance(), 0, 2);
            body.Controls.Add(navigation, 0, 0); body.Controls.Add(workspaces, 1, 0); root.Controls.Add(body, 0, 1);
            captureWorkspace = new TabPage("采集工作台") { Name = "CaptureWorkspace", Padding = new Padding(0) }; captureWorkspace.Controls.Add(captureBody); workspaces.TabPages.Add(captureWorkspace);
            logManagementWorkspace = new TabPage("日志管理") { Name = "LogManagementTab", Padding = new Padding(0) }; logManagementWorkspace.Controls.Add(BuildLogManagement()); workspaces.TabPages.Add(logManagementWorkspace);
            workspaces.SelectedIndexChanged += delegate { UpdateCyberNavigation(); if (workspaces.SelectedTab == logManagementWorkspace) EnsureLogList(); };
            workspaces.Selecting += delegate(object sender, TabControlCancelEventArgs args) { if (logsDeleting && args.TabPage != logManagementWorkspace) args.Cancel = true; };
            footer.Text = "原始证据完整保留  /  采集结果与证据完整性分别呈现"; footer.Dock = DockStyle.Fill; footer.TextAlign = ContentAlignment.MiddleLeft;
            root.Controls.Add(footer, 0, 2); Controls.Add(root);
        }
        void FinishCyberWindow() {
            finishingCyber = true;
            var evidence = FindCyberControl(this, "EvidenceTabs") as TabControl;
            foreach (var name in new[] { "ConfigurationTab", "IndexedEvidenceTab", "CausalityTab", "McpTab", "CasePromptTab" }) {
                TabPage page = null;
                foreach (TabPage candidate in config.TabPages) if (candidate.Name == name) page = candidate;
                if (page != null) config.TabPages.Remove(page);
                if (page == null && evidence != null) {
                    foreach (TabPage candidate in evidence.TabPages) if (candidate.Name == name) page = candidate;
                    if (page != null) evidence.TabPages.Remove(page);
                }
                if (page != null) workspaces.TabPages.Add(page);
            }
            foreach (TabPage page in config.TabPages) captureSection.Items.Add(page.Text);
            config.SelectedIndex = 0; captureSection.SelectedIndex = 0;
            captureSection.SelectedIndexChanged += delegate { if (captureSection.SelectedIndex >= 0) config.SelectedIndex = captureSection.SelectedIndex; };
            config.SelectedIndexChanged += delegate { if (captureSection.SelectedIndex != config.SelectedIndex) captureSection.SelectedIndex = config.SelectedIndex; };
            var number = 0;
            foreach (TabPage page in workspaces.TabPages) {
                var selected = page;
                var button = new CyberNavigationButton { Name = "Navigate_" + page.Name, Text = (++number).ToString("00") + "   " + page.Text, Width = 168, Height = 43, Margin = new Padding(0, 0, 0, 6), AccessibleName = page.Text, FlatStyle = FlatStyle.Flat, TabStop = true };
                button.Click += delegate { workspaces.SelectedTab = selected; }; workspaceNavigation.Add(page, button); cyberNavigation.Controls.Add(button);
            }
            workspaces.SelectedIndex = 0;
            finishingCyber = false; ApplyCyberTheme(false); ApplyResponsiveCyberLayout(); UpdateCyberNavigation();
        }
        Control BuildAppearance() {
            var panel = new TableLayoutPanel { Name = "ThemeSettings", Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 6, Margin = new Padding(0), Padding = new Padding(0, 8, 0, 0) };
            panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            foreach (var height in new[] { 24, 32, 32, 32, 34, 38 }) panel.RowStyles.Add(new RowStyle(SizeType.Absolute, height));
            panel.Controls.Add(new Label { Text = "外观 / APPEARANCE", Dock = DockStyle.Fill, Font = new Font("Consolas", 8.5F), Margin = new Padding(0), TextAlign = ContentAlignment.MiddleLeft }, 0, 0);
            foreach (var theme in ThemePreferences.Themes) themeSelector.Items.Add(theme);
            themeSelector.Margin = new Padding(0, 0, 0, 6);
            themeSelector.SelectedItem = themePreferences.Current; panel.Controls.Add(themeSelector, 0, 1);
            var colors = new TableLayoutPanel { Name = "ThemeColorActions", Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Margin = new Padding(0) }; colors.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50)); colors.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            colors.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            primaryColor.Margin = new Padding(0, 0, 4, 6); secondaryColor.Margin = new Padding(4, 0, 0, 6);
            colors.Controls.Add(primaryColor, 0, 0); colors.Controls.Add(secondaryColor, 1, 0); panel.Controls.Add(colors, 0, 2);
            var reset = new Button { Name = "ThemeResetButton", Text = "恢复当前主题配色", Dock = DockStyle.Fill, Margin = new Padding(0, 0, 0, 6) }; panel.Controls.Add(reset, 0, 3);
            themeMotion.Checked = themePreferences.Motion; panel.Controls.Add(themeMotion, 0, 4); panel.Controls.Add(themeStatus, 0, 5);
            themeSelector.SelectedIndexChanged += delegate { if (changingTheme || themeSelector.SelectedItem == null) return; themePreferences.Selected = ((ThemeDefinition)themeSelector.SelectedItem).Id; ApplyCyberTheme(true); };
            primaryColor.Click += delegate { ChooseThemeColor(true); }; secondaryColor.Click += delegate { ChooseThemeColor(false); };
            reset.Click += delegate { themePreferences.ResetCurrent(); ApplyCyberTheme(true); };
            themeMotion.CheckedChanged += delegate { if (changingTheme) return; themePreferences.Motion = themeMotion.Checked; UpdateThemeAnimation(); SaveThemePreference(); };
            themeAnimation.Tick += delegate {
                if (!Visible || WindowState == FormWindowState.Minimized || !CyberPalette.Phantom) { UpdateThemeAnimation(); return; }
                if (cyberHeader != null) { cyberHeader.AnimationPhase = themeAnimationClock.Elapsed.TotalSeconds / 5.0; cyberHeader.Invalidate(CyberPalette.Theme.Id == "phantom-smoke" ? cyberHeader.ClientRectangle : new Rectangle(0, Math.Max(0, cyberHeader.Height - 24), cyberHeader.Width, 24)); }
            };
            Shown += delegate { UpdateThemeAnimation(); }; VisibleChanged += delegate { UpdateThemeAnimation(); }; Resize += delegate { UpdateThemeAnimation(); ApplyResponsiveCyberLayout(); };
            return panel;
        }
        void ChooseThemeColor(bool primary) {
            using (var dialog = new ColorDialog { Color = primary ? themePreferences.Primary : themePreferences.Secondary, FullOpen = true, AnyColor = true }) {
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                themePreferences.SetColors(primary ? dialog.Color : themePreferences.Primary, primary ? themePreferences.Secondary : dialog.Color); ApplyCyberTheme(true);
            }
        }
        void ApplyCyberTheme(bool save) {
            changingTheme = true; SuspendLayout();
            try {
                CyberPalette.Theme = themePreferences.Current; CyberPalette.Primary = themePreferences.Primary; CyberPalette.Secondary = themePreferences.Secondary;
                ink = CyberPalette.Text; accent = CyberPalette.Cyan;
                CyberPalette.Apply(this); BackColor = CyberPalette.Background; cyberNavigation.BackColor = CyberPalette.Background;
                if (cyberHeader != null) foreach (Control content in cyberHeader.Controls) content.BackColor = Color.Transparent;
                start.BackColor = CyberPalette.Primary; start.ForeColor = CyberPalette.ButtonText(CyberPalette.Primary);
                stop.ForeColor = CyberPalette.Danger; pauseRecording.ForeColor = CyberPalette.Cyan;
                foreach (var button in workspaceNavigation.Values) { button.BackColor = CyberPalette.Background; button.FlatAppearance.BorderSize = 0; }
                foreach (Control control in Controls.Find("CyberRoot", true)) control.BackColor = CyberPalette.Background;
                footer.ForeColor = CyberPalette.Muted; phase.ForeColor = CyberPalette.Cyan; themeStatus.ForeColor = CyberPalette.Muted;
                themeCaption.Text = "ALCOHOL   /   V8 TRACE / 浏览器采集"; themeCaption.ForeColor = CyberPalette.Pink;
                foreach (Control control in Controls.Find("BrandTitle", true)) { control.Text = "V8 TRACE / 浏览器采集"; control.ForeColor = CyberPalette.Theme.Dark ? CyberPalette.Cyan : CyberPalette.Text; }
                if (themePreferences.Current.Id == "classic-simple") foreach (var metric in new[] { eventCount, byteCount, gapCount, elapsed }) metric.ForeColor = CyberPalette.Text;
                primaryColor.BackColor = CyberPalette.Primary; primaryColor.ForeColor = CyberPalette.ButtonText(CyberPalette.Primary); primaryColor.AccessibleDescription = ThemePreferences.Hex(CyberPalette.Primary);
                secondaryColor.BackColor = CyberPalette.Secondary; secondaryColor.ForeColor = CyberPalette.ButtonText(CyberPalette.Secondary); secondaryColor.AccessibleDescription = ThemePreferences.Hex(CyberPalette.Secondary);
                StyleLogActions(); foreach (DataGridViewRow row in logGrid.Rows) if (row.Tag is LogStorageItem && !((LogStorageItem)row.Tag).CanClean) row.DefaultCellStyle.ForeColor = CyberPalette.Muted;
                RefreshCyberEdition(false); ApplyOwnTitleBar();
                var evidence = FindCyberControl(this, "EvidenceTabs") as TabControl; if (evidence != null) StyleEvidenceNavigation(evidence);
                if (save) SaveThemePreference();
                else if (!String.IsNullOrEmpty(themePreferences.Warning)) themeStatus.Text = themePreferences.Warning;
            } finally { ResumeLayout(true); changingTheme = false; UpdateThemeAnimation(); Invalidate(true); }
        }
        void SaveThemePreference() { try { themePreferences.Save(); themeStatus.Text = "已保存 · 下次启动自动恢复。"; } catch (Exception error) { themeStatus.Text = "本次已应用；保存失败：" + error.Message; } }
        Font compactMetricFont, fullMetricFont;
        void QueueCyberNavigationLayout() {
            if (navigationLayoutPending || !IsHandleCreated || closing || IsDisposed) return;
            int width = Math.Max(90, cyberNavigation.ClientSize.Width - cyberNavigation.Padding.Horizontal), height = ClientSize.Height < 760 ? 35 : 43;
            if (workspaceNavigation.Values.All(button => button.Width == width && button.Height == height)) return;
            navigationLayoutPending = true;
            BeginInvoke((MethodInvoker)delegate { navigationLayoutPending = false; if (!closing && !IsDisposed) ApplyResponsiveCyberLayout(); });
        }
        void ResizeCyberNavigationButtons() {
            int width = Math.Max(90, cyberNavigation.ClientSize.Width - cyberNavigation.Padding.Horizontal), height = ClientSize.Height < 760 ? 35 : 43;
            cyberNavigation.SuspendLayout();
            try { foreach (var button in workspaceNavigation.Values) { if (button.Width != width) button.Width = width; if (button.Height != height) button.Height = height; } }
            finally { cyberNavigation.ResumeLayout(true); }
        }
        void ApplyResponsiveCyberLayout() {
            var compact = ClientSize.Width < 1140;
            if (compactMetricFont == null) { compactMetricFont = new Font("Segoe UI", 15F, FontStyle.Bold); fullMetricFont = new Font("Segoe UI", 19F, FontStyle.Bold); }
            foreach (var metric in new[] { eventCount, byteCount, gapCount, elapsed }) metric.Font = compact ? compactMetricFont : fullMetricFont;
            category.Width = compact ? 90 : 124; filter.Width = compact ? 85 : 175; refresh.Width = compact ? 92 : 124;
            ResizeCyberNavigationButtons();
        }
        void UpdateThemeAnimation() {
            if (IsDisposed || Disposing) return;
            var active = themePreferences != null && themePreferences.Motion && CyberPalette.Phantom && Visible && !closing && WindowState != FormWindowState.Minimized && CyberPalette.SystemAllowsMotion;
            themeMotion.Enabled = true;
            themeMotion.StateText = !themePreferences.Motion ? "已关闭" : !CyberPalette.Phantom ? "静态主题" : !CyberPalette.SystemAllowsMotion ? "遵循系统" : active ? "已开启" : "已暂停";
            themeMotion.AccessibleDescription = "动态光效" + (themePreferences.Motion ? "偏好已开启。" : "偏好已关闭。") + themeMotion.StateText + "；只重绘顶部装饰区域，最小化或系统关闭动画时自动停止。";
            if (active) { if (!themeAnimationClock.IsRunning) themeAnimationClock.Start(); themeAnimation.Start(); } else { themeAnimation.Stop(); themeAnimationClock.Stop(); }
            if (cyberHeader != null) { cyberHeader.Animate = active; cyberHeader.Invalidate(); }
        }
        static Control FindCyberControl(Control root, string name) {
            if (root.Name == name) return root;
            foreach (Control child in root.Controls) { var found = FindCyberControl(child, name); if (found != null) return found; }
            return null;
        }
        void UpdateCyberNavigation() {
            if (finishingCyber) return;
            foreach (var item in workspaceNavigation) { item.Value.Selected = item.Key == workspaces.SelectedTab; item.Value.Invalidate(); }
        }
        void RefreshCyberEdition(bool known) {
            cyberEditionKnown |= known;
            editionBadge.Text = cyberEditionKnown ? (advancedAnalysis ? "PROFESSIONAL" : "COMMUNITY") + "  /  ALCOHOL" : "ENGINE / CONNECTING";
            editionBadge.ForeColor = advancedAnalysis ? CyberPalette.Pink : CyberPalette.Cyan;
            editionBadge.BackColor = CyberPalette.Input;
        }
        [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);
        protected override void WndProc(ref Message message) { base.WndProc(ref message); if ((message.Msg == 0x001A || message.Msg == 0x031A) && themePreferences != null) UpdateThemeAnimation(); }
        protected override void OnHandleCreated(EventArgs args) {
            base.OnHandleCreated(args);
            ApplyOwnTitleBar();
        }
        void ApplyOwnTitleBar() { if (!IsHandleCreated) return; try { int dark = CyberPalette.Theme.Dark ? 1 : 0; if (DwmSetWindowAttribute(Handle, 20, ref dark, 4) != 0) DwmSetWindowAttribute(Handle, 19, ref dark, 4); } catch (Exception) { } }
    }
}
