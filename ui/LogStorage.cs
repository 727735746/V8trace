using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace V8Trace {
    public sealed class LogStorageItem {
        public string Path, Name, State, Stamp;
        public bool Directory, CanClean, SizeKnown;
        public long Bytes;
        public DateTime ModifiedUtc;
    }
    public sealed class LogStorageSnapshot {
        public string Root, Warning;
        public bool Truncated;
        public readonly List<LogStorageItem> Items = new List<LogStorageItem>();
        public long KnownBytes { get { return Items.Where(x => x.SizeKnown).Sum(x => x.Bytes); } }
    }
    public sealed class LogCleanupResult {
        public int Removed, Skipped, Failed;
        public bool Cancelled;
        public readonly List<string> Details = new List<string>();
    }
    public sealed class LogStorage {
        public const long LargeBytes = 1073741824;
        readonly string installation, audit;
        readonly Action<string, bool> remove;
        static readonly string[] projectFiles = { "CMakeLists.txt", "CMakeCache.txt", "build.ninja", "package.json", "pyproject.toml", "Cargo.toml", ".gclient" };

        public LogStorage(string installationRoot, string auditFile, Action<string, bool> remover = null) {
            installation = Canonical(installationRoot); audit = Canonical(auditFile); remove = remover ?? Remove;
        }
        public static string Canonical(string value) {
            if (String.IsNullOrWhiteSpace(value)) throw new IOException("请先选择日志目录。");
            if (value.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) value = @"\\" + value.Substring(8);
            else if (value.StartsWith(@"\\?\", StringComparison.Ordinal)) value = value.Substring(4);
            var path = System.IO.Path.GetFullPath(value);
            var root = System.IO.Path.GetPathRoot(path);
            return path.Length > root.Length ? path.TrimEnd(System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar) : root;
        }
        public static bool Within(string path, string parent) {
            path = Canonical(path); parent = Canonical(parent);
            return String.Equals(path, parent, StringComparison.OrdinalIgnoreCase) || path.StartsWith(parent.TrimEnd('\\', '/') + System.IO.Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
        }
        static bool Overlaps(string left, string right) { return Within(left, right) || Within(right, left); }
        static void NoLinks(string path) {
            for (var current = Canonical(path); !String.IsNullOrEmpty(current); current = System.IO.Path.GetDirectoryName(current)) {
                if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0) throw new IOException("链接或重解析路径保持原样：" + current);
                if (String.Equals(current, System.IO.Path.GetPathRoot(current), StringComparison.OrdinalIgnoreCase)) break;
            }
        }
        static bool Project(string directory) {
            return System.IO.Directory.Exists(System.IO.Path.Combine(directory, ".git")) || projectFiles.Any(name => File.Exists(System.IO.Path.Combine(directory, name))) ||
                System.IO.Directory.EnumerateFiles(directory).Any(ProgramContent);
        }
        static bool ProgramContent(string path) { return new[] { ".exe", ".dll", ".sys", ".sln", ".csproj", ".vcxproj", ".cpp", ".cc", ".h", ".cs", ".ps1", ".cmd", ".bat" }.Contains(System.IO.Path.GetExtension(path), StringComparer.OrdinalIgnoreCase); }
        void Guard(string root) {
            root = Canonical(root); NoLinks(root);
            if (!System.IO.Directory.Exists(root)) throw new IOException("日志目录不存在，或当前账户没有读取权限。");
            if (String.Equals(root, System.IO.Path.GetPathRoot(root), StringComparison.OrdinalIgnoreCase) || Within(installation, root)) throw new IOException("此目录属于磁盘根或软件上级目录，仅供浏览，清理操作已锁定。");
            var special = new[] { Environment.SpecialFolder.UserProfile, Environment.SpecialFolder.DesktopDirectory, Environment.SpecialFolder.MyDocuments,
                Environment.SpecialFolder.LocalApplicationData, Environment.SpecialFolder.ApplicationData, Environment.SpecialFolder.Windows,
                Environment.SpecialFolder.ProgramFiles, Environment.SpecialFolder.ProgramFilesX86, Environment.SpecialFolder.System };
            foreach (var folder in special) {
                var value = Environment.GetFolderPath(folder);
                if (!String.IsNullOrEmpty(value) && String.Equals(root, Canonical(value), StringComparison.OrdinalIgnoreCase)) throw new IOException("系统或个人主目录仅供浏览，请选择其中具体的日志子目录。");
            }
            foreach (var folder in new[] { Environment.SpecialFolder.Windows, Environment.SpecialFolder.ProgramFiles, Environment.SpecialFolder.ProgramFilesX86, Environment.SpecialFolder.System }) {
                var value = Environment.GetFolderPath(folder);
                if (!String.IsNullOrEmpty(value) && Within(root, value)) throw new IOException("系统与程序安装目录保持原样，请选择独立的日志目录。");
            }
            if (String.Equals(root, Canonical(System.IO.Path.GetTempPath()), StringComparison.OrdinalIgnoreCase) || Project(root)) throw new IOException("临时目录根或工程目录仅供浏览，请选择具体日志子目录。");
        }
        List<string> Protected(IEnumerable<string> values) {
            var result = new List<string> { audit, System.IO.Path.Combine(installation, "runtime"), System.IO.Path.Combine(installation, "chromium"), System.IO.Path.Combine(installation, "app"), System.IO.Path.Combine(installation, "verification") };
            if (values != null) foreach (var value in values) if (!String.IsNullOrWhiteSpace(value)) result.Add(Canonical(value));
            return result;
        }
        static void Fingerprint(HashAlgorithm hash, string path) {
            var info = new FileInfo(path);
            var value = path + "\0" + ((int)File.GetAttributes(path)).ToString() + "\0" + File.GetCreationTimeUtc(path).Ticks + "\0" + File.GetLastWriteTimeUtc(path).Ticks + "\0" + (System.IO.Directory.Exists(path) ? 0 : info.Length) + "\n";
            var bytes = Encoding.UTF8.GetBytes(value); hash.TransformBlock(bytes, 0, bytes.Length, bytes, 0);
        }
        LogStorageItem Inspect(string path, List<string> protectedPaths, CancellationToken stop) {
            stop.ThrowIfCancellationRequested();
            var item = new LogStorageItem { Path = Canonical(path), Name = System.IO.Path.GetFileName(path), State = "可清理", CanClean = true, SizeKnown = true };
            try {
                var attributes = File.GetAttributes(path); item.Directory = (attributes & FileAttributes.Directory) != 0;
                item.ModifiedUtc = File.GetLastWriteTimeUtc(path);
                if ((attributes & (FileAttributes.ReparsePoint | FileAttributes.System)) != 0) throw new IOException("链接 / 系统项，保留");
                if (!item.Directory && ProgramContent(path)) throw new IOException("程序 / 工程文件，保留");
                if (protectedPaths.Any(value => Overlaps(path, value))) throw new IOException("当前使用中，保留");
                long count = 0;
                using (var fingerprint = SHA256.Create()) {
                Fingerprint(fingerprint, path);
                var pending = new Stack<string>();
                if (item.Directory) pending.Push(path);
                else { item.Bytes = new FileInfo(path).Length; count = 1; }
                while (pending.Count > 0) {
                    stop.ThrowIfCancellationRequested();
                    var directory = pending.Pop();
                    if (Project(directory)) throw new IOException("含工程 / 构建数据，保留");
                    var children = System.IO.Directory.EnumerateFileSystemEntries(directory).Take((int)(200001 - count)).OrderBy(value => value, StringComparer.OrdinalIgnoreCase);
                    foreach (var child in children) {
                        stop.ThrowIfCancellationRequested();
                        if (++count > 200000) throw new IOException("目录条目过多，请进入子目录管理");
                        var flags = File.GetAttributes(child);
                        if ((flags & (FileAttributes.ReparsePoint | FileAttributes.System)) != 0) throw new IOException("含链接 / 系统项，保留");
                        if (protectedPaths.Any(value => Overlaps(child, value))) throw new IOException("含使用中数据，保留");
                        Fingerprint(fingerprint, child);
                        if ((flags & FileAttributes.Directory) != 0) pending.Push(child);
                        else item.Bytes = checked(item.Bytes + new FileInfo(child).Length);
                    }
                }
                fingerprint.TransformFinalBlock(new byte[0], 0, 0);
                item.Stamp = BitConverter.ToString(fingerprint.Hash).Replace("-", "");
                }
            } catch (OperationCanceledException) { throw; }
            catch (Exception error) { item.CanClean = false; item.SizeKnown = false; item.State = error is UnauthorizedAccessException ? "读取受限，保留" : error.Message; }
            return item;
        }
        public LogStorageSnapshot Scan(string root, IEnumerable<string> protectedPaths, CancellationToken stop) {
            root = Canonical(root);
            var snapshot = new LogStorageSnapshot { Root = root };
            if (!System.IO.Directory.Exists(root)) { snapshot.Warning = "目录不存在或没有读取权限；请选择其他日志目录。"; return snapshot; }
            NoLinks(root);
            try { Guard(root); } catch (Exception error) { snapshot.Warning = error.Message; }
            var protectedList = Protected(protectedPaths);
            foreach (var path in System.IO.Directory.EnumerateFileSystemEntries(root)) {
                stop.ThrowIfCancellationRequested();
                if (snapshot.Items.Count >= 10000) { snapshot.Truncated = true; snapshot.Warning = "条目超过 10000，请进入更具体的子目录；全部清理暂时锁定。"; break; }
                LogStorageItem item;
                if (String.IsNullOrEmpty(snapshot.Warning)) item = Inspect(path, protectedList, stop);
                else {
                    item = new LogStorageItem { Path = Canonical(path), Name = System.IO.Path.GetFileName(path), CanClean = false, SizeKnown = false, State = "目录保护，保留" };
                    try { item.Directory = (File.GetAttributes(path) & FileAttributes.Directory) != 0; item.ModifiedUtc = File.GetLastWriteTimeUtc(path); } catch (IOException) { } catch (UnauthorizedAccessException) { }
                }
                snapshot.Items.Add(item);
            }
            snapshot.Items.Sort((a, b) => { var kind = b.Directory.CompareTo(a.Directory); return kind != 0 ? kind : StringComparer.CurrentCultureIgnoreCase.Compare(a.Name, b.Name); });
            return snapshot;
        }
        void Audit(string state, string root, string target, bool recycle, string error) {
            System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(audit));
            var value = new Dictionary<string, object> { { "time", DateTimeOffset.UtcNow.ToString("o") }, { "state", state }, { "root", root }, { "path", target }, { "recycle", recycle }, { "error", error } };
            var bytes = Encoding.UTF8.GetBytes(new JavaScriptSerializer().Serialize(value) + "\n");
            using (var stream = new FileStream(audit, FileMode.Append, FileAccess.Write, FileShare.Read)) { stream.Write(bytes, 0, bytes.Length); stream.Flush(true); }
        }
        public LogCleanupResult Clean(LogStorageSnapshot snapshot, IEnumerable<string> selected, IEnumerable<string> protectedPaths, bool recycle, CancellationToken stop) {
            if (snapshot == null) throw new IOException("请先刷新日志列表。");
            Guard(snapshot.Root);
            var result = new LogCleanupResult(); var protectedList = Protected(protectedPaths);
            var wanted = new HashSet<string>(selected.Select(Canonical), StringComparer.OrdinalIgnoreCase);
            var listed = snapshot.Items.ToDictionary(item => item.Path, StringComparer.OrdinalIgnoreCase);
            foreach (var target in wanted) {
                if (stop.IsCancellationRequested) { result.Cancelled = true; break; }
                try {
                    if (!String.Equals(Canonical(System.IO.Path.GetDirectoryName(target)), snapshot.Root, StringComparison.OrdinalIgnoreCase) || !listed.ContainsKey(target)) throw new IOException("目标不属于当前已列出的日志目录。");
                    var original = listed[target]; var current = Inspect(target, protectedList, stop);
                    if (!original.CanClean || !current.CanClean || original.Stamp != current.Stamp) { ++result.Skipped; result.Details.Add(original.Name + "：使用中、受保护或已有变化，请刷新。"); continue; }
                    NoLinks(target); Audit("requested", snapshot.Root, target, recycle, "");
                    remove(target, recycle);
                    if (File.Exists(target) || System.IO.Directory.Exists(target)) throw new IOException("条目仍存在，已保留失败信息。");
                    ++result.Removed;
                    try { Audit("removed", snapshot.Root, target, recycle, ""); } catch (Exception error) { result.Details.Add(System.IO.Path.GetFileName(target) + "：已移除，结果审计追加失败：" + error.Message); }
                } catch (OperationCanceledException) { result.Cancelled = true; break; }
                catch (Exception error) { ++result.Failed; result.Details.Add(System.IO.Path.GetFileName(target) + "：" + error.Message); try { Audit("failed", snapshot.Root, target, recycle, error.Message); } catch (Exception) { } }
            }
            return result;
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct ShellOperation {
            public IntPtr Window; public uint Function;
            [MarshalAs(UnmanagedType.LPWStr)] public string From;
            [MarshalAs(UnmanagedType.LPWStr)] public string To;
            public ushort Flags;
            [MarshalAs(UnmanagedType.Bool)] public bool Aborted;
            public IntPtr Mappings;
            [MarshalAs(UnmanagedType.LPWStr)] public string Title;
        }
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern int SHFileOperation(ref ShellOperation operation);
        static void Remove(string path, bool recycle) {
            if (!recycle) { if (System.IO.Directory.Exists(path)) System.IO.Directory.Delete(path, true); else File.Delete(path); return; }
            var operation = new ShellOperation { Function = 3, From = path + '\0' + '\0', Flags = 0x40 | 0x10 | 0x4 | 0x400 };
            var error = SHFileOperation(ref operation);
            if (operation.Aborted) throw new OperationCanceledException("清理已取消，剩余条目保持原样。");
            if (error != 0) throw new IOException("回收站操作失败，条目保持原样（代码 " + error + "）。");
        }
    }
}
