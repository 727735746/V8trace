[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackagePath,[Parameter(Mandatory=$true)][string]$EvidencePath)
if ($PSVersionTable.PSEdition -ne 'Desktop') {
    & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -STA -File $PSCommandPath -PackagePath $PackagePath -EvidencePath $EvidencePath
    exit $LASTEXITCODE
}
$ErrorActionPreference='Stop'
$package=(Resolve-Path -LiteralPath $PackagePath).Path
$evidence=[IO.Path]::GetFullPath($EvidencePath)
New-Item -ItemType Directory -Path $evidence -Force|Out-Null
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('v8trace-studio-ui-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary|Out-Null
$source=@'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using V8Trace;
public static class StudioSmoke {
    static readonly JavaScriptSerializer json=new JavaScriptSerializer{MaxJsonLength=16*1024*1024};
    static string root,evidence; static int checks;
    static void Check(bool value,string message){++checks;if(!value)throw new Exception(message);}
    static T Find<T>(Form form,string name) where T:Control {var controls=form.Controls.Find(name,true);Check(controls.Length==1,"Missing "+name);return (T)controls[0];}
    static object Invoke(Form form,string name,params object[] values){return typeof(TraceWindow).GetMethod(name,BindingFlags.Instance|BindingFlags.NonPublic).Invoke(form,values);}
    static object Field(Form form,string name){return typeof(TraceWindow).GetField(name,BindingFlags.Instance|BindingFlags.NonPublic).GetValue(form);}
    static void Set(Form form,string name,object value){typeof(TraceWindow).GetField(name,BindingFlags.Instance|BindingFlags.NonPublic).SetValue(form,value);}
    static void Write(string name,object value){File.WriteAllText(Path.Combine(evidence,name),json.Serialize(value),new UTF8Encoding(false));}
    static void FixtureWrite(string file,object value){File.WriteAllText(file,json.Serialize(value),new UTF8Encoding(false));}
    static string Hash(byte[] bytes){using(var sha=SHA256.Create())return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-","").ToLowerInvariant();}
    static bool Idle(Form form){return Field(form,"studioRequestId")==null&&!(bool)Field(form,"busy");}
    static void LayoutReport(Form form){var chain=new List<object>();for(Control control=Find<RichTextBox>(form,"IndexedDetails");control!=null;control=control.Parent)chain.Add(new{name=control.Name,type=control.GetType().Name,bounds=control.Bounds.ToString(),client=control.ClientRectangle.ToString()});Write("layout-1000.json",chain);Snapshot(form,"index-1000.png");}
    static void BoundsVisible(Control control,Control boundary){var rect=control.RectangleToScreen(control.ClientRectangle);var original=rect;var chain=new List<object>();for(var parent=control.Parent;parent!=null;parent=parent.Parent){var bounds=parent.RectangleToScreen(parent.ClientRectangle);chain.Add(new{name=parent.Name,type=parent.GetType().Name,bounds=bounds.ToString()});rect=Rectangle.Intersect(rect,bounds);if(parent==boundary)break;}bool visible=rect.Width>=original.Width-1&&rect.Height>=original.Height-1;if(!visible)Write("clipped-control.json",new{control=control.Name,original=original.ToString(),effective=rect.ToString(),chain=chain});Check(visible,"Clipped control "+control.Name+" "+original+" => "+rect);}
    [DllImport("user32.dll")]static extern bool PrintWindow(IntPtr window,IntPtr dc,uint flags);
    static void Snapshot(Form form,string name){using(var bitmap=new Bitmap(form.Width,form.Height))using(var graphics=Graphics.FromImage(bitmap)){var dc=graphics.GetHdc();try{Check(PrintWindow(form.Handle,dc,2),"PrintWindow");}finally{graphics.ReleaseHdc(dc);}bitmap.Save(Path.Combine(evidence,name),System.Drawing.Imaging.ImageFormat.Png);}}
    [STAThread]public static int Main(string[] args){
        root=args[0];evidence=args[1];TraceWindow form=null;var timer=new Timer{Interval=80};bool entered=false;int stage=0;string failure=null;var started=DateTime.UtcNow;
        try{
            var capture=Path.Combine(root,"capture");Directory.CreateDirectory(Path.Combine(capture,"events"));Directory.CreateDirectory(Path.Combine(capture,"catalog"));Directory.CreateDirectory(Path.Combine(capture,"blobs","sha256"));
            var bytes=Encoding.UTF8.GetBytes("//中文源码\nfunction 计算(x){return x+1;}\n");var digest=Hash(bytes);File.WriteAllBytes(Path.Combine(capture,"blobs","sha256",digest),bytes);
            var reference=new Dictionary<string,object>{{"path","blobs/sha256/"+digest},{"sha256",digest},{"bytes",bytes.Length},{"representation","script-source-utf8"}};
            FixtureWrite(Path.Combine(capture,"summary.json"),new{schemaVersion=1,captureConclusion="collected",integrityConclusion="complete",eventCount=1,recordedBytes=3000000,criticalGapCount=0,counts=new{jscall=1}});
            File.WriteAllText(Path.Combine(capture,"events","jscall.jsonl"),json.Serialize(new{seq=1,category="jscall",kind="call-enter",payload=new{scriptUrl="https://fixture.test/中文.js",scriptId=7,functionName="计算",functionId="9",requestId="req-1",recordingWindow=2,processId=32,isolateId="0x1",source=reference,value=new string('中',1000000)}})+"\n",new UTF8Encoding(false));
            File.WriteAllText(Path.Combine(capture,"catalog","scripts.jsonl"),json.Serialize(new{scriptId=7,url="https://fixture.test/中文.js",processId=32,isolateId="0x1",source=reference})+"\n",new UTF8Encoding(false));
            Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);form=new TraceWindow{ShowInTaskbar=false,StartPosition=FormStartPosition.Manual,Location=new Point(-5000,-5000)};
            timer.Tick+=(sender,eventArgs)=>{
                if(entered)return;entered=true;
                try{
                    Check((DateTime.UtcNow-started).TotalSeconds<120,"Studio UI timeout at stage "+stage);
                    if(stage==0){
                        if(!Find<Button>(form,"StartButton").Enabled)return;
                        Check((bool)Field(form,"studioAvailable"),"Real C++ studio protocol");
                        var raw=new Dictionary<string,object>{{"url","https://fixture.test/"},{"output",@".\logs\中文-new"},{"chromePath",@".\chromium\chrome.exe"},{"transport","cdp"},{"http",true},{"maxBytes",123456789L},{"customLongText",new string('中',20000)+"😀"}};
                        Invoke(form,"ApplyConfigurationDocument",raw,null);var first=(Dictionary<string,object>)Invoke(form,"PortableOptions");
                        Check(json.Serialize(first)==json.Serialize(raw),"Exact import/export: relative paths, missing keys, byte budget, unknown Chinese field");
                        Find<NumericUpDown>(form,"DurationInput").Value=11;var changed=(Dictionary<string,object>)Invoke(form,"PortableOptions");Check(Convert.ToInt32(changed["durationSeconds"])==11&&Convert.ToInt64(changed["maxBytes"])==123456789L,"Only changed control overwrites imported precision");
                        Invoke(form,"SetBusy",true);Check(!Find<TextBox>(form,"VmpProbeFileInput").Enabled&&!Find<Button>(form,"ConfigImportButton").Enabled,"Busy configuration and VM inputs locked");Invoke(form,"SetBusy",false);
                        Find<TabControl>(form,"WorkspaceTabs").SelectedTab=Find<TabPage>(form,"ConfigurationTab");Find<Button>(form,"ConfigCurrentButton").PerformClick();Snapshot(form,"configuration-1280.png");
                        Set(form,"captureDirectory",capture);Find<TabControl>(form,"WorkspaceTabs").SelectedTab=Find<TabPage>(form,"IndexedEvidenceTab");Invoke(form,"BuildIndex");stage=1;
                    }else if(stage==1&&Idle(form)&&Find<DataGridView>(form,"IndexGrid").Rows.Count==3){
                        Check(Field(form,"viewerGeneration")!=null&&Find<RichTextBox>(form,"IndexedDetails").Text.Length>10,"Real indexed read rendered");Find<ComboBox>(form,"IndexCategoryInput").Text="jscall";Invoke(form,"QueryIndex",false);stage=2;
                    }else if(stage==2&&Idle(form)){
                        Check(Find<DataGridView>(form,"IndexGrid").Rows.Count==1&&Find<Button>(form,"ValueNextButton").Enabled,"Giant event is paged, not eagerly loaded");
                        Check(Find<RichTextBox>(form,"IndexedDetails").Text.Length<70000,"UI details bounded");Check(Find<Label>(form,"ElapsedLabel").Text=="00:00","Analysis tasks must not invent a capture duration");Snapshot(form,"index-1280.png");form.Size=new Size(1000,720);form.PerformLayout();Application.DoEvents();LayoutReport(form);foreach(var key in new[]{"script","function","request","window","process","isolate","producer","call"})BoundsVisible(Find<TextBox>(form,"Index"+key+"Filter"),Find<TabPage>(form,"IndexedEvidenceTab"));Check(Find<RichTextBox>(form,"IndexedDetails").Height>=50,"Narrow details pane collapsed");BoundsVisible(Find<RichTextBox>(form,"IndexedDetails"),Find<TabPage>(form,"IndexedEvidenceTab"));Snapshot(form,"index-1000.png");Find<Button>(form,"ValueNextButton").PerformClick();stage=3;
                    }else if(stage==3&&Idle(form)){
                        Check(Convert.ToInt64(Field(form,"valueOffset"))==65536,"Next byte page");Invoke(form,"JumpIndexed","script");stage=4;
                    }else if(stage==4&&Idle(form)){
                        Check(Find<DataGridView>(form,"IndexGrid").Rows.Count==2,"Script jump returns call and exact source observations");Invoke(form,"SelectBlob");stage=5;
                    }else if(stage==5&&Idle(form)){
                        Check(Find<RichTextBox>(form,"IndexedDetails").Text.Contains("function 计算"),"Actual source blob read");
                        var raw=(Dictionary<string,object>)Invoke(form,"CaptureOptions");raw["chromePath"]=Path.Combine(root,"missing-chrome.exe");Invoke(form,"RequestPreflight",raw,false);stage=6;
                    }else if(stage==6&&Idle(form)){
                        Check(Find<Label>(form,"ConfigurationStatus").Text.Contains("失败")&&!Directory.Exists(Path.Combine(root,"logs","中文-new")),"Missing browser preflight did not launch or create capture");
                        Find<Button>(form,"CloseIndexButton").PerformClick();stage=7;
                    }else if(stage==7&&Idle(form)){
                        Check(Field(form,"viewerGeneration")==null&&Find<DataGridView>(form,"IndexGrid").Rows.Count==0,"Index close reset UI");
                        Write("ui-studio.json",new{conclusion="PASS",checks=checks,browserStarted=false,configurationRoundtrip=true,indexedRealWorker=true,giantValuePaged=true,sourceBlobRead=true});stage=8;timer.Stop();Invoke(form,"Send","shutdown",null);form.Close();
                    }
                }catch(Exception error){failure=error.ToString();timer.Stop();try{Invoke(form,"Send","shutdown",null);}catch{}form.Close();}finally{entered=false;}
            };
            timer.Start();Application.Run(form);Check(failure==null,failure);Check(stage==8,"UI ended early");return 0;
        }catch(Exception error){Write("ui-studio.json",new{conclusion="FAIL",checks=checks,error=error.ToString()});return 1;}finally{timer.Dispose();if(form!=null)form.Dispose();}
    }
}
'@
try {
    $sourceFile=Join-Path $temporary 'studio-smoke.cs'
    $exe=Join-Path $temporary 'studio-smoke.exe'
    [IO.File]::WriteAllText($sourceFile,$source,[Text.UTF8Encoding]::new($false))
    & "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:exe /platform:x64 /codepage:65001 ('/out:'+$exe) /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll ('/reference:'+(Join-Path $package 'V8Trace.exe')) $sourceFile
    if($LASTEXITCODE -ne 0){throw 'Studio smoke compiler failed'}
    Copy-Item -LiteralPath (Join-Path $package 'V8Trace.exe') -Destination $temporary
    Copy-Item -LiteralPath (Join-Path $package 'runtime') -Destination (Join-Path $temporary 'runtime') -Recurse
    Copy-Item -LiteralPath (Join-Path $package 'app') -Destination (Join-Path $temporary 'app') -Recurse
    & $exe $temporary $evidence
    if($LASTEXITCODE -ne 0){throw 'Studio UI smoke failed; see ui-studio.json'}
    Write-Output 'PASS C++ worker configuration and indexed UI smoke'
}finally{
    $resolved=[IO.Path]::GetFullPath($temporary)
    if((Split-Path -Parent $resolved) -ne [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') -or (Split-Path -Leaf $resolved) -notmatch '^v8trace-studio-ui-[a-f0-9]{32}$'){throw 'Owned UI cleanup boundary mismatch'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
    [IO.File]::WriteAllText((Join-Path $evidence 'cleanup.json'),'{"temporaryFixturesRemoved":true}',[Text.UTF8Encoding]::new($false))
}
