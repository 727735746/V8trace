[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackagePath,[Parameter(Mandatory=$true)][string]$EvidencePath)
if ($PSVersionTable.PSEdition -ne 'Desktop') {
    & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -STA -File $PSCommandPath -PackagePath $PackagePath -EvidencePath $EvidencePath
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackagePath).Path
$evidence = [IO.Path]::GetFullPath($EvidencePath)
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('v8trace-cpp-ui-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
$source = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using V8Trace;
public static class CppUiTcpSmoke {
    static JavaScriptSerializer json = new JavaScriptSerializer();
    static T Find<T>(Form form,string name) where T:Control { var found=form.Controls.Find(name,true); Check(found.Length==1,"Missing "+name); return (T)found[0]; }
    static void Check(bool ok,string reason) { if(!ok) throw new Exception(reason); }
    static void Invoke(Form form,string name,params object[] args) { typeof(TraceWindow).GetMethod(name,BindingFlags.NonPublic|BindingFlags.Instance).Invoke(form,args); }
    static string Hash(string file) { using(var sha=SHA256.Create()) using(var input=File.OpenRead(file)) return BitConverter.ToString(sha.ComputeHash(input)).Replace("-","").ToLowerInvariant(); }
    static void Write(string file,object value) { File.WriteAllText(file,json.Serialize(value),new UTF8Encoding(false)); }
    [STAThread] public static int Main(string[] args) {
        var package=args[0];var evidence=args[1];var temporary=args[2];Directory.SetCurrentDirectory(package);
        Environment.SetEnvironmentVariable("NODE_OPTIONS","--v8trace-must-not-use-node");
        var capture=Path.Combine(temporary,"capture");Directory.CreateDirectory(capture);
        Write(Path.Combine(capture,"summary.json"),new{schemaVersion=1,backend="chromium-native",captureId="ui-cpp-fixture",captureConclusion="collected",integrityConclusion="complete",eventCount=1,recordedBytes=8388608,criticalGapCount=0,counts=new{jscall=1}});
        var payload=Path.Combine(capture,"full-log.bin");var bytes=new byte[8*1024*1024+1];for(int i=0;i<bytes.Length;i++)bytes[i]=(byte)(i%251);File.WriteAllBytes(payload,bytes);
        var listener=new TcpListener(IPAddress.Loopback,0);listener.Start();var port=((IPEndPoint)listener.LocalEndpoint).Port;listener.Stop();
        var received=Path.Combine(temporary,"received");var runtime=Path.Combine(package,"runtime","v8trace-runtime.exe");
        var server=Process.Start(new ProcessStartInfo{FileName=runtime,Arguments="receive --root \""+received+"\" --port "+port+" --once",UseShellExecute=false,CreateNoWindow=true,WindowStyle=ProcessWindowStyle.Hidden,RedirectStandardOutput=true,RedirectStandardError=true});
        TraceWindow form=null;string failure=null;int stage=0;var start=DateTime.UtcNow;var timer=new Timer{Interval=100};
        try {
            Check(server.StandardOutput.ReadLine().Contains("listening"),"TCP server not ready");
            Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
            form=new TraceWindow();form.ShowInTaskbar=false;form.StartPosition=FormStartPosition.Manual;form.Location=new Point(-5000,-5000);
            timer.Tick+=(sender,eventArgs)=>{try {
                if((DateTime.UtcNow-start).TotalSeconds>60)throw new Exception("UI timeout at stage "+stage+": "+Find<RichTextBox>(form,"SummaryText").Text);
                if(stage==0&&Find<Button>(form,"StartButton").Enabled) {
                    Check(Find<Label>(form,"PhaseLabel").Text.Contains("C++"),"C++ worker not active");
                    Check(Find<CheckedListBox>(form,"NativeKindsInput").Items.Count==7,"Native filters missing");
                    Check(Find<TabControl>(form,"ConfigTabs").TabPages.Contains(Find<TabPage>(form,"TcpTransferTab"))&&Find<TabControl>(form,"WorkspaceTabs").TabPages.Contains(Find<TabPage>(form,"ConfigurationTab")),"TCP/configuration tabs missing");
                    Check(Find<NumericUpDown>(form,"WriteBufferInput").Value==8&&Find<NumericUpDown>(form,"WriteBatchInput").Value==1024,"Lossless buffer defaults missing");
                    var calls=Find<CheckBox>(form,"Channel_jsCallDetail");var values=Find<CheckBox>(form,"JsCallValuesOption");var filtering=Find<CheckBox>(form,"DetailFilteringOption");
                    Check(values.Checked,"Full JSCall values must remain the default");calls.Checked=true;Check(values.Enabled,"JSCall values option disabled");filtering.Checked=true;
                    foreach(var key in new[]{"jsValueScriptInclude","jsValueScriptExclude","jsValueFunctionInclude","jsValueFunctionExclude"})Check(Find<TextBox>(form,key+"Input").Enabled,"Value scope input missing or disabled");
                    values.Checked=false;Check(!Find<TextBox>(form,"jsValueScriptIncludeInput").Enabled,"Structure-only mode must disable value ranges");
                    Check(Find<TextBox>(form,"detailScriptIncludeInput").Enabled,"Structure-only mode must retain independent call range");
                    values.Checked=true;filtering.Checked=false;calls.Checked=false;
                    Invoke(form,"Send","inspect",new Dictionary<string,object>{{"capture",capture}});stage=1;
                } else if(stage==1&&Find<Button>(form,"TcpSendButton").Enabled) {
                    Find<TabControl>(form,"ConfigTabs").SelectedTab=Find<TabPage>(form,"TcpTransferTab");
                    Find<TextBox>(form,"TcpHostInput").Text="127.0.0.1";Find<NumericUpDown>(form,"TcpPortInput").Value=port;
                    Find<Button>(form,"TcpSendButton").PerformClick();Check(!Find<TextBox>(form,"TcpHostInput").Enabled,"TCP inputs not locked");stage=2;
                } else if(stage==2&&Find<Label>(form,"TcpStatusLabel").Text.Contains("SHA-256")) {
                    Check(Find<Button>(form,"StartButton").Enabled,"UI did not leave busy state");
                    Check(server.WaitForExit(5000)&&server.ExitCode==0,"TCP server did not commit");
                    string copied=null;foreach(var dir in Directory.GetDirectories(received))if(Path.GetFileName(dir)!=".incoming")copied=Path.Combine(dir,"full-log.bin");
                    Check(copied!=null&&Hash(copied)==Hash(payload),"TCP UI transfer integrity mismatch");
                    using(var bitmap=new Bitmap(form.Width,form.Height)){form.DrawToBitmap(bitmap,new Rectangle(Point.Empty,form.Size));bitmap.Save(Path.Combine(evidence,"tcp-ui.png"),System.Drawing.Imaging.ImageFormat.Png);}
                    Write(Path.Combine(evidence,"ui-verification.json"),new{conclusion="PASS",runtime="cpp",nodeRequired=false,nativeKinds=7,tabs=Find<TabControl>(form,"ConfigTabs").TabPages.Count,jsCallLayering=true,valueScopeInputs=4,fullValuesDefault=true,losslessBufferInputs=true,tcpUiTransfer=true,sourceRetained=File.Exists(payload),bytes=bytes.Length,sha256=Hash(copied),browserStarted=false});
                    timer.Stop();stage=3;Invoke(form,"Send","shutdown",null);form.Close();
                }
            }catch(Exception error){failure=error.ToString();timer.Stop();try{Invoke(form,"Send","shutdown",null);}catch{}form.Close();}};
            timer.Start();Application.Run(form);Check(failure==null,failure);Check(stage==3,"UI exited early");return 0;
        } catch(Exception error) { File.WriteAllText(Path.Combine(evidence,"ui-error.txt"),error.ToString(),new UTF8Encoding(false));return 1; }
        finally {timer.Dispose();if(form!=null)form.Dispose();if(!server.HasExited){server.Kill();server.WaitForExit();}server.Dispose();}
    }
}
'@
try {
    $sourceFile = Join-Path $temporary 'ui-smoke.cs'
    $exe = Join-Path $temporary 'ui-smoke.exe'
    [IO.File]::WriteAllText($sourceFile,$source,[Text.UTF8Encoding]::new($false))
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $compiler /nologo /target:exe /platform:x64 /codepage:65001 ('/out:'+$exe) /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll ('/reference:'+(Join-Path $package 'V8Trace.exe')) $sourceFile
    if ($LASTEXITCODE -ne 0) { throw 'UI test compiler failed' }
    Copy-Item -LiteralPath (Join-Path $package 'V8Trace.exe') -Destination (Join-Path $temporary 'V8Trace.exe')
    $env:V8TRACE_UI_TEST_PACKAGE = $package
    Copy-Item -LiteralPath (Join-Path $package 'runtime') -Destination (Join-Path $temporary 'runtime') -Recurse
    Copy-Item -LiteralPath (Join-Path $package 'app') -Destination (Join-Path $temporary 'app') -Recurse
    & $exe $package $evidence $temporary
    if ($LASTEXITCODE -ne 0) { throw 'UI TCP test failed; see ui-error.txt' }
    Write-Output 'PASS C++ GUI worker, seven filters and actual multi-block TCP UI transfer'
} finally {
    $resolved = [IO.Path]::GetFullPath($temporary)
    $parent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if ((Split-Path -Parent $resolved) -ne $parent -or (Split-Path -Leaf $resolved) -notmatch '^v8trace-cpp-ui-[a-f0-9]{32}$') { throw 'Temporary cleanup boundary mismatch' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
    [IO.File]::WriteAllText((Join-Path $evidence 'cleanup.json'),'{"temporaryFixturesRemoved":true}',[Text.UTF8Encoding]::new($false))
}
