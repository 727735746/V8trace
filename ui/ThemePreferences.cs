using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace V8Trace {
    public sealed class ThemeDefinition {
        public readonly string Id, Name, Caption;
        public readonly bool Dark, Multicolor;
        public readonly Color Background, Surface, Input, Border, Text, Muted, Primary, Secondary;
        public ThemeDefinition(string id, string name, string caption, bool dark, bool multicolor, int background, int surface, int input, int border, int text, int muted, int primary, int secondary) {
            Id = id; Name = name; Caption = caption; Dark = dark; Multicolor = multicolor;
            Background = Rgb(background); Surface = Rgb(surface); Input = Rgb(input); Border = Rgb(border); Text = Rgb(text); Muted = Rgb(muted); Primary = Rgb(primary); Secondary = Rgb(secondary);
        }
        static Color Rgb(int value) { return Color.FromArgb(255, Color.FromArgb(value)); }
        public override string ToString() { return Name; }
    }
    public sealed class ThemeColors { public string Primary, Secondary; }
    public sealed class ThemePreferences {
        public const string DefaultTheme = "phantom";
        public static readonly ThemeDefinition[] Themes = {
            new ThemeDefinition("phantom", "赛博朋克 · 幻影", "PHANTOM / NEON SIGNAL", true, false, 0x090811, 0x141321, 0x1D1B30, 0x45405F, 0xEEE9FF, 0xACA5C5, 0x56BCFF, 0xF05DE0),
            new ThemeDefinition("phantom-spectrum", "赛博朋克 · 幻影炫彩", "SPECTRUM / ELECTRIC MIRAGE", true, true, 0x070710, 0x111225, 0x1A1A35, 0x414168, 0xF0EBFF, 0xAFA8CB, 0x37DCFF, 0xFF48DE),
            new ThemeDefinition("polar-terminal", "极夜终端", "TERMINAL / PURE SIGNAL", true, false, 0x080E0C, 0x101B17, 0x16251E, 0x354D40, 0xE1F4E8, 0x9BB8A6, 0x70EAA5, 0xD4E89A),
            new ThemeDefinition("aurora-ocean", "极光深海", "AURORA / DEEP CURRENT", true, false, 0x08171F, 0x102632, 0x173441, 0x3B5E6C, 0xDFF5FA, 0x99BAC7, 0x60E1CE, 0x83AEFF),
            new ThemeDefinition("obsidian-gold", "曜石鎏金", "OBSIDIAN / PRECISION IN GOLD", true, false, 0x151311, 0x211F1A, 0x2C2922, 0x635641, 0xF3EEE2, 0xBAAF99, 0xF2CE83, 0xD9A181),
            new ThemeDefinition("frost-lab", "霜白实验室", "FROST / CLEAR EVIDENCE", false, false, 0xEAF0F7, 0xFAFCFF, 0xF0F4FA, 0xBCCBDE, 0x20324A, 0x52677F, 0x1666B8, 0x8045AA),
            new ThemeDefinition("classic-simple", "经典简洁 · 原版", "V8 TRACE / 浏览器采集", false, false, 0xF1F5F8, 0xFFFFFF, 0xF8FAFC, 0xC4CFD8, 0x192A3D, 0x596B7D, 0x007578, 0x466E96),
            new ThemeDefinition("phantom-smoke", "赛博朋克 · 幻影烟雾", "PHANTOM SMOKE / SPECTRAL FLOW", true, true, 0x080912, 0x121422, 0x1B1E32, 0x444A68, 0xEDF2FF, 0xA6B2CD, 0x65D9FF, 0xC487FF)
        };
        readonly string file;
        public string Selected = DefaultTheme;
        public bool Motion = true;
        public readonly Dictionary<string, ThemeColors> Colors = new Dictionary<string, ThemeColors>(StringComparer.Ordinal);
        public string Warning { get; private set; }
        sealed class Record { public string Selected; public Dictionary<string, ThemeColors> Colors; public bool? Motion; }
        public static string DefaultPath { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Alcohol", "V8Trace", "ui-theme.json"); } }
        ThemePreferences(string path) { file = Path.GetFullPath(path); }
        public static ThemeDefinition Definition(string id) { return Themes.FirstOrDefault(theme => theme.Id == id) ?? Themes[0]; }
        public static bool TryColor(string value, out Color color) {
            color = Color.Empty; int rgb;
            if (value == null || value.Length != 7 || value[0] != '#' || !Int32.TryParse(value.Substring(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out rgb)) return false;
            color = Color.FromArgb(255, Color.FromArgb(rgb)); return true;
        }
        public static string Hex(Color color) { return "#" + (color.ToArgb() & 0xffffff).ToString("X6", CultureInfo.InvariantCulture); }
        public static ThemePreferences Load(string path) {
            var preferences = new ThemePreferences(path);
            try {
                if (!File.Exists(preferences.file)) return preferences;
                if (new FileInfo(preferences.file).Length > 16384) throw new IOException("主题偏好文件过大，已使用默认主题。");
                var record = new JavaScriptSerializer { MaxJsonLength = 16384 }.Deserialize<Record>(File.ReadAllText(preferences.file, Encoding.UTF8));
                if (record == null) throw new IOException("主题偏好为空。");
                preferences.Selected = Definition(record.Selected).Id;
                preferences.Motion = record.Motion ?? true;
                if (record.Colors != null) foreach (var theme in Themes) {
                    ThemeColors colors; Color primary, secondary;
                    if (record.Colors.TryGetValue(theme.Id, out colors) && colors != null && TryColor(colors.Primary, out primary) && TryColor(colors.Secondary, out secondary)) preferences.Colors[theme.Id] = new ThemeColors { Primary = Hex(primary), Secondary = Hex(secondary) };
                }
            } catch (Exception error) {
                if (!(error is IOException || error is UnauthorizedAccessException || error is ArgumentException || error is InvalidOperationException)) throw;
                preferences.Selected = DefaultTheme; preferences.Colors.Clear(); preferences.Warning = "主题偏好读取失败，已恢复幻影默认值：" + error.Message;
            }
            return preferences;
        }
        public ThemeDefinition Current { get { return Definition(Selected); } }
        public Color Primary { get { Color value; ThemeColors colors; return Colors.TryGetValue(Current.Id, out colors) && TryColor(colors.Primary, out value) ? value : Current.Primary; } }
        public Color Secondary { get { Color value; ThemeColors colors; return Colors.TryGetValue(Current.Id, out colors) && TryColor(colors.Secondary, out value) ? value : Current.Secondary; } }
        public void SetColors(Color primary, Color secondary) { Colors[Current.Id] = new ThemeColors { Primary = Hex(primary), Secondary = Hex(secondary) }; }
        public void ResetCurrent() { Colors.Remove(Current.Id); }
        public void Save() {
            Directory.CreateDirectory(Path.GetDirectoryName(file));
            var temporary = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try {
                var data = new JavaScriptSerializer().Serialize(new Record { Selected = Current.Id, Colors = Colors, Motion = Motion });
                using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None)) { var bytes = new UTF8Encoding(false).GetBytes(data + "\n"); stream.Write(bytes, 0, bytes.Length); stream.Flush(true); }
                for (int attempt = 0; ; ++attempt) {
                    try { if (File.Exists(file)) File.Replace(temporary, file, null); else File.Move(temporary, file); break; }
                    catch (IOException) { if (attempt >= 4) throw; Thread.Sleep(30 * (attempt + 1)); }
                }
                Warning = null;
            } finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }
    }
}
