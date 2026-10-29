[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$PackagePath,
    [Parameter(Mandatory=$true)][string]$EvidencePath,
    [Parameter(Mandatory=$true)][ValidateSet('community','professional')][string]$Edition
)
if ($PSVersionTable.PSEdition -ne 'Desktop') {
    & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -STA -File $PSCommandPath -PackagePath $PackagePath -EvidencePath $EvidencePath -Edition $Edition
    exit $LASTEXITCODE
}
$ErrorActionPreference='Stop'
$package=(Resolve-Path -LiteralPath $PackagePath).Path
$evidence=[IO.Path]::GetFullPath($EvidencePath)
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('v8trace-edition-ui-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
$source=@'
using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using V8Trace;
public static class EditionSmoke {
    static int checks;
    static bool success;
    static string error="", evidence, edition;
    static readonly JavaScriptSerializer json=new JavaScriptSerializer();
    static void Check(bool value,string message){checks++;if(!value)throw new Exception(message);}
    static T Find<T>(Form form,string name) where T:Control {var items=form.Controls.Find(name,true);Check(items.Length==1,"Control "+name);return (T)items[0];}
    static object Invoke(Form form,string method,params object[] args){return typeof(TraceWindow).GetMethod(method,BindingFlags.NonPublic|BindingFlags.Instance).Invoke(form,args);}
    static Dictionary<string,object> Record(object item){return (Dictionary<string,object>)item.GetType().GetField("Record").GetValue(item);}
    static string Id(object item){return Convert.ToString(Record(item)["id"]);}
    static string FileHash(string file){using(var hash=System.Security.Cryptography.SHA256.Create())return BitConverter.ToString(hash.ComputeHash(File.ReadAllBytes(file))).Replace("-","").ToLowerInvariant();}
    [DllImport("user32.dll")]static extern bool PrintWindow(IntPtr window,IntPtr dc,uint flags);
    static void Snapshot(Form form,string name){using(var bitmap=new Bitmap(form.Width,form.Height))using(var graphics=Graphics.FromImage(bitmap)){var dc=graphics.GetHdc();try{Check(PrintWindow(form.Handle,dc,2),"Actual UI screenshot");}finally{graphics.ReleaseHdc(dc);}bitmap.Save(Path.Combine(evidence,name),System.Drawing.Imaging.ImageFormat.Png);}}
    static void Gates(Form form){
        bool pro=edition=="professional";
        foreach(var key in new[]{"opcode","wasm","vmpProbe","httpRootTrace","domTrace","cryptoTrace"})Check(Find<CheckBox>(form,"Channel_"+key).Enabled==pro,"Compiled availability "+key);
        Check(Find<Button>(form,"CausalBuild").Enabled==pro,"Advanced graph button");
        foreach(var key in new[]{"http","scripts","hooks","jsCallDetail","nativeDetail","objectSnapshots","wasmModules","envAbsentProbe","envInterfaceProbe","envHostMutation","envInventory","webSocket","cookies","storage","indexedDB"})Check(Find<CheckBox>(form,"Channel_"+key).Enabled,"Community evidence retained "+key);
    }
    [STAThread]public static int Main(string[] args){
        evidence=args[0];edition=args[1];Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
        using(var form=new TraceWindow())using(var timer=new Timer{Interval=100}){
            var deadline=DateTime.UtcNow.AddSeconds(45);
            timer.Tick+=(sender,eventArgs)=>{try{
                if(DateTime.UtcNow>deadline)throw new Exception("Actual worker readiness timeout");
                if(!(bool)typeof(TraceWindow).GetField("ready",BindingFlags.NonPublic|BindingFlags.Instance).GetValue(form))return;
                timer.Stop();Check(form.Text.Contains(edition=="professional"?"Professional":"Community")&&form.Text.Contains("Alcohol"),"Actual backend edition title");
                var transport=Find<ComboBox>(form,"TransportInput");transport.SelectedIndex=1;Gates(form);
                Invoke(form,"SetBusy",true);Invoke(form,"SetBusy",false);Gates(form);
                transport.SelectedIndex=0;Check(Find<CheckBox>(form,"Channel_opcode").Enabled==(edition=="professional"),"CDP does not re-enable professional collection");transport.SelectedIndex=1;Gates(form);
                var presets=Find<ComboBox>(form,"PresetInput");Check(presets.Items.Count==22,"21 scenarios plus custom selection");int blocked=0;
                for(int i=1;i<presets.Items.Count;i++){var record=Record(presets.Items[i]);if(record.ContainsKey("available")&&!Convert.ToBoolean(record["available"])){blocked++;presets.SelectedIndex=i;Check(!Find<Button>(form,"ApplyPresetButton").Enabled,"Unavailable preset cannot apply");}}
                Check(blocked==(edition=="professional"?0:4),"Exact professional preset count");
                foreach(var id in new[]{"env-protocol","env-target-detail"}){
                    int at=-1;for(int i=1;i<presets.Items.Count;i++)if(Id(presets.Items[i])==id)at=i;Check(at>=0,"Environment scenario "+id);
                    Find<TextBox>(form,"PresetScriptScopeInput").Text="fixture.js";presets.SelectedIndex=at;Check(Find<Button>(form,"ApplyPresetButton").Enabled,"Complete environment preset enabled");Find<Button>(form,"ApplyPresetButton").PerformClick();
                    var options=(Dictionary<string,object>)Invoke(form,"CaptureOptions");
                    foreach(var key in new[]{"http","scripts","hooks","redirectBodies","jsCallValues"})Check(Convert.ToBoolean(options[key]),"Environment values retained "+key);
                    if(id=="env-target-detail"){Check(Convert.ToBoolean(options["jsCallDetail"])&&Convert.ToBoolean(options["nativeDetail"]),"Full targeted JSCall and Native remain selected");Check(Convert.ToString(options["detailScriptInclude"])=="fixture.js","Exact target scope");}
                }
                Snapshot(form,"edition-environment.png");
                var opcode=Find<CheckBox>(form,"Channel_opcode");
                for(Control parent=opcode.Parent;parent!=null;parent=parent.Parent){var scroll=parent as ScrollableControl;if(scroll!=null)scroll.ScrollControlIntoView(opcode);}
                Snapshot(form,"edition-channels.png");success=true;form.Close();
            }catch(Exception failure){timer.Stop();error=failure.ToString();form.Close();}};
            form.Shown+=(sender,eventArgs)=>timer.Start();Application.Run(form);
        }
        File.WriteAllText(Path.Combine(evidence,"ui-verification.json"),json.Serialize(new{protocol="v8trace/ui-edition-check/1",edition=edition,status=success?"passed":"failed",checks=checks,error=error,browserLaunched=false,uiSha256=FileHash(typeof(TraceWindow).Assembly.Location),runtimeSha256=FileHash(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"runtime","v8trace-runtime.exe"))}),new UTF8Encoding(false));return success?0:1;
    }
}
'@
try {
    $code=Join-Path $temporary 'EditionSmoke.cs';[IO.File]::WriteAllText($code,$source,[Text.UTF8Encoding]::new($false))
    $exe=Join-Path $temporary 'EditionSmoke.exe'
    & "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:exe /platform:x64 /codepage:65001 ('/out:'+$exe) /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll ('/reference:'+(Join-Path $package 'V8Trace.exe')) $code
    if($LASTEXITCODE -ne 0){throw 'Edition UI test compilation failed'}
    Copy-Item -LiteralPath (Join-Path $package 'V8Trace.exe') -Destination $temporary
    Copy-Item -LiteralPath (Join-Path $package 'runtime') -Destination (Join-Path $temporary 'runtime') -Recurse
    Copy-Item -LiteralPath (Join-Path $package 'app') -Destination (Join-Path $temporary 'app') -Recurse
    & $exe $evidence $Edition
    if($LASTEXITCODE -ne 0){throw 'Actual edition UI test failed; see ui-verification.json'}
} finally {
    $resolved=[IO.Path]::GetFullPath($temporary);$parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if((Split-Path -Parent $resolved) -ne $parent -or (Split-Path -Leaf $resolved) -notmatch '^v8trace-edition-ui-[a-f0-9]{32}$'){throw 'Temporary cleanup boundary mismatch'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
    [IO.File]::WriteAllText((Join-Path $evidence 'cleanup.json'),'{"temporaryFixturesRemoved":true}',[Text.UTF8Encoding]::new($false))
}
