using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using V8Trace;

public static class StorageThemeTests {
    static int checks;
    static void Check(bool condition, string description) { ++checks; if (!condition) throw new Exception(description); }
    static string Write(string path, string contents) { Directory.CreateDirectory(Path.GetDirectoryName(path)); File.WriteAllText(path, contents, new UTF8Encoding(false)); return path; }
    static LogStorageItem Item(LogStorageSnapshot snapshot, string name) { return snapshot.Items.Single(item => item.Name == name); }
    public static int Main(string[] args) {
        var root = Path.GetFullPath(args[0]); var evidence = Path.GetFullPath(args[1]);
        try {
            var settings = Path.Combine(root, "settings", "theme.json");
            var preferences = ThemePreferences.Load(settings);
            Check(preferences.Selected == "phantom" && !File.Exists(settings), "Default phantom without writing user state");
            Check(ThemePreferences.Themes.Length == 8 && ThemePreferences.Themes.Select(theme => theme.Id).Distinct().Count() == 8 && ThemePreferences.Themes.Any(theme => theme.Id == "phantom-smoke"), "Eight independent themes including original simple style and Phantom Smoke");
            foreach (var theme in ThemePreferences.Themes) {
                preferences.Selected = theme.Id; preferences.SetColors(Color.Black, Color.White); preferences.Save();
                var loaded = ThemePreferences.Load(settings);
                Check(loaded.Selected == theme.Id && loaded.Primary.ToArgb() == Color.Black.ToArgb() && loaded.Secondary.ToArgb() == Color.White.ToArgb(), "Per-theme custom colors roundtrip " + theme.Id);
                CyberPalette.Theme = theme; CyberPalette.Primary = Color.Black; CyberPalette.Secondary = Color.White;
                Check(CyberPalette.Contrast(CyberPalette.Cyan, theme.Surface) >= 4.5 && CyberPalette.Contrast(CyberPalette.Pink, theme.Surface) >= 4.5, "Readable accent text " + theme.Id);
                Check(CyberPalette.Contrast(theme.Text, theme.Surface) >= 4.5 && CyberPalette.Contrast(theme.Muted, theme.Surface) >= 4.5, "Readable body and secondary text " + theme.Id);
                Check(CyberPalette.Contrast(CyberPalette.ButtonText(theme.Primary), theme.Primary) >= 4.5, "Readable filled button " + theme.Id);
                Check(CyberPalette.Contrast(CyberPalette.Danger, theme.Surface) >= 4.5, "Semantic danger color " + theme.Id);
            }
            preferences.Selected = "phantom-spectrum"; preferences.ResetCurrent(); preferences.Save();
            preferences.Motion = false; preferences.Save(); Check(!ThemePreferences.Load(settings).Motion, "Persist explicit motion-off preference");
            Check(ThemePreferences.Load(settings).Primary.ToArgb() == ThemePreferences.Definition("phantom-spectrum").Primary.ToArgb(), "Reset current palette");
            preferences.Selected = "phantom"; Check(preferences.Primary.ToArgb() == Color.Black.ToArgb(), "Reset preserves other palettes");
            Check(!Directory.GetFiles(Path.GetDirectoryName(settings), "*.tmp").Any(), "Atomic settings leave no temporary files");
            Write(settings, "{bad"); Check(ThemePreferences.Load(settings).Selected == "phantom" && ThemePreferences.Load(settings).Warning != null, "Corrupt preferences restore default");
            Write(settings, "{\"Selected\":\"unknown\",\"Colors\":{\"phantom\":{\"Primary\":\"#GGGGGG\",\"Secondary\":\"#000000\"},\"injected\":{\"Primary\":\"#ffffff\",\"Secondary\":\"#ffffff\"}}}");
            Check(ThemePreferences.Load(settings).Selected == "phantom" && ThemePreferences.Load(settings).Colors.Count == 0, "Whitelist theme IDs and color syntax");
            Write(settings, new string('x', 16385)); Check(ThemePreferences.Load(settings).Warning != null, "Bounded preferences input");
            Color color; Check(!ThemePreferences.TryColor("#ffffff00", out color) && !ThemePreferences.TryColor("transparent", out color) && ThemePreferences.TryColor("#56BCFF", out color), "Strict opaque RGB color contract");

            var logs = Path.Combine(root, "managed"); var audit = Path.Combine(root, "audit", "events.jsonl"); var install = Path.Combine(root, "installation");
            Directory.CreateDirectory(install);
            var storage = new LogStorage(install, audit);
            Write(Path.Combine(logs, "first", "trace.jsonl"), "one\ntwo\n"); Write(Path.Combine(logs, "second", "summary.json"), "{}");
            var active = Write(Path.Combine(logs, "active", "trace.jsonl"), "active");
            var project = Write(Path.Combine(logs, "project", "CMakeLists.txt"), "fixture");
            var app = Write(Path.Combine(logs, "software", "keep.exe"), "fixture-not-executable");
            var snapshot = storage.Scan(logs, new[] { Path.GetDirectoryName(active) }, CancellationToken.None);
            Check(Item(snapshot, "first").CanClean && Item(snapshot, "first").Bytes == 8, "Directory size and cleanup eligibility");
            Check(!Item(snapshot, "active").CanClean && !Item(snapshot, "project").CanClean && !Item(snapshot, "software").CanClean, "Active, project and program protections");
            Check(!Item(snapshot, "junction").CanClean && !Item(snapshot, "junction").SizeKnown, "Do not follow junctions");
            var result = storage.Clean(snapshot, new[] { Path.Combine(logs, "first") }, new[] { Path.GetDirectoryName(active) }, false, CancellationToken.None);
            Check(result.Removed == 1 && result.Failed == 0 && !Directory.Exists(Path.Combine(logs, "first")) && Directory.Exists(Path.Combine(logs, "second")), "Selected-only permanent deletion on owned fixture");
            Check(File.ReadAllLines(audit).Length == 2, "Durable request and removal audits");
            var protectedResult = storage.Clean(snapshot, new[] { Path.GetDirectoryName(active), Path.GetDirectoryName(project), Path.GetDirectoryName(app), Path.Combine(logs, "junction") }, new[] { Path.GetDirectoryName(active) }, false, CancellationToken.None);
            Check(protectedResult.Skipped == 4 && File.Exists(active) && File.Exists(project) && File.Exists(app), "Revalidate protections before removal");
            Write(Path.Combine(logs, "changed", "a.jsonl"), "before"); Write(Path.Combine(logs, "changed", "z.jsonl"), "future");
            File.SetLastWriteTimeUtc(Path.Combine(logs, "changed", "z.jsonl"), DateTime.UtcNow.AddYears(1));
            snapshot = storage.Scan(logs, new[] { Path.GetDirectoryName(active) }, CancellationToken.None);
            Write(Path.Combine(logs, "changed", "a.jsonl"), "change"); File.SetLastWriteTimeUtc(Path.Combine(logs, "changed", "a.jsonl"), DateTime.UtcNow.AddMinutes(5));
            result = storage.Clean(snapshot, new[] { Path.Combine(logs, "changed") }, null, false, CancellationToken.None);
            Check(result.Skipped == 1 && Directory.Exists(Path.Combine(logs, "changed")), "Detect changed child even below newest timestamp and same byte length");
            result = storage.Clean(snapshot, new[] { Path.Combine(root, "outside.txt") }, null, false, CancellationToken.None);
            Check(result.Failed == 1, "Reject targets outside immediate listed root");
            var cancel = new CancellationTokenSource(); cancel.Cancel(); result = storage.Clean(snapshot, new[] { Path.Combine(logs, "second") }, null, false, cancel.Token);
            Check(result.Cancelled && Directory.Exists(Path.Combine(logs, "second")), "Cancellation preserves remaining data"); cancel.Dispose();
            bool recycleRequested = false;
            var recycling = new LogStorage(install, audit, delegate(string target, bool recycle) { recycleRequested = recycle; Directory.Move(target, Path.Combine(root, "recycled-fixture")); });
            snapshot = recycling.Scan(logs, null, CancellationToken.None); result = recycling.Clean(snapshot, new[] { Path.Combine(logs, "second") }, null, true, CancellationToken.None);
            Check(recycleRequested && result.Removed == 1 && File.Exists(Path.Combine(root, "recycled-fixture", "summary.json")), "Recycle flag reaches remover; no real recycle bin touched");
            Write(Path.Combine(logs, "failure", "trace.jsonl"), "keep"); bool permanentFallback = false;
            var failing = new LogStorage(install, audit, delegate(string target, bool recycle) { if (!recycle) permanentFallback = true; throw new IOException("fixture recycle error"); });
            snapshot = failing.Scan(logs, null, CancellationToken.None); result = failing.Clean(snapshot, new[] { Path.Combine(logs, "failure") }, null, true, CancellationToken.None);
            Check(result.Failed == 1 && !permanentFallback && Directory.Exists(Path.Combine(logs, "failure")), "Recycle failure has no permanent fallback");
            var drive = storage.Scan(Path.GetPathRoot(root), null, CancellationToken.None);
            Check(drive.Warning != null && drive.Items.All(item => !item.CanClean && !item.SizeKnown), "Protected root metadata only, no recursive sizing");
            Check(storage.Scan(install, null, CancellationToken.None).Warning != null, "Installation root protected");
            Check(LogStorage.Within(Path.Combine(logs, "a"), logs) && !LogStorage.Within(logs + "-other", logs), "Canonical path segment boundary");
            Check(LogStorage.LargeBytes == 1073741824L, "One GiB threshold without allocating large fixtures");
            File.WriteAllText(evidence, new JavaScriptSerializer().Serialize(new { protocol = "v8trace/ui-storage-theme-tests/1", status = "passed", checks = checks, realUserLogsTouched = false, realRecycleBinTouched = false }), new UTF8Encoding(false));
            Console.WriteLine("PASS " + checks + " checks"); return 0;
        } catch (Exception error) {
            File.WriteAllText(evidence, new JavaScriptSerializer().Serialize(new { status = "failed", checks = checks, error = error.ToString() }), new UTF8Encoding(false)); Console.Error.WriteLine(error); return 1;
        }
    }
}
