[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackagePath,[Parameter(Mandatory=$true)][string]$EvidencePath,[switch]$SkipClipboard)
if ($PSVersionTable.PSEdition -ne 'Desktop') {
    $extra=@();if($SkipClipboard){$extra+='-SkipClipboard'}
    & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -STA -File $PSCommandPath -PackagePath $PackagePath -EvidencePath $EvidencePath @extra
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackagePath).Path
$evidence = [IO.Path]::GetFullPath($EvidencePath)
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('v8trace-catalog-ui-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
$source = @'
using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using V8Trace;
public static class CatalogSmoke {
    static readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = 8*1024*1024 };
    static int checks,appliedPresets,blockedPresets,promptCount;
    static bool professional;
    static string evidence,temporary;
    static readonly string[] rules={"detailScriptInclude","detailScriptExclude","jsFunctionInclude","jsFunctionExclude","nativeNameInclude","nativeNameExclude","jsValueScriptInclude","jsValueScriptExclude","jsValueFunctionInclude","jsValueFunctionExclude"};
    static readonly Dictionary<string,string> numbers=new Dictionary<string,string>{{"durationSeconds","DurationInput"},{"maxEvents","EventsBudgetInput"},{"maxBytes","BytesBudgetInput"},{"maxBodyBytes","BodyBudgetInput"},{"objectDepth","ObjectDepthInput"},{"objectNodes","ObjectNodesInput"},{"objectProperties","ObjectPropertiesInput"},{"writeBufferBytes","WriteBufferInput"},{"writeBatchBytes","WriteBatchInput"}};
    static T Find<T>(Form f,string name) where T:Control { var found=f.Controls.Find(name,true); Check(found.Length==1,"Missing "+name); return (T)found[0]; }
    static void Check(bool ok,string reason) { checks++;if(!ok)throw new Exception(reason); }
    static object Invoke(Form f,string name,params object[] args) { return typeof(TraceWindow).GetMethod(name,BindingFlags.NonPublic|BindingFlags.Instance).Invoke(f,args); }
    static Dictionary<string,object> Record(object value) { return (Dictionary<string,object>)value.GetType().GetField("Record",BindingFlags.Instance|BindingFlags.Public).GetValue(value); }
    static Dictionary<string,object> Options(Form f) { return (Dictionary<string,object>)Invoke(f,"CaptureOptions"); }
    static string SerializeOptions(Form f) { return json.Serialize(Options(f)); }
    static void Write(string name,object data) { File.WriteAllText(Path.Combine(evidence,name),json.Serialize(data),new UTF8Encoding(false)); }
    static T RetryClipboard<T>(Func<T> action) { Exception last=null;for(int i=0;i<8;i++){try{return action();}catch(System.Runtime.InteropServices.ExternalException error){last=error;System.Threading.Thread.Sleep(75);}}throw last; }
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd,IntPtr dc,uint flags);
    static void Snapshot(Form f,string name) { using(var bitmap=new Bitmap(f.Width,f.Height))using(var graphics=Graphics.FromImage(bitmap)){var dc=graphics.GetHdc();try{Check(PrintWindow(f.Handle,dc,2),"PrintWindow");}finally{graphics.ReleaseHdc(dc);}bitmap.Save(Path.Combine(evidence,name),System.Drawing.Imaging.ImageFormat.Png);} }
    static void RecordingControls(TraceWindow f) {
        var enabled=Find<CheckBox>(f,"RecordingControlOption");var initial=Find<CheckBox>(f,"StartPausedOption");var toggle=Find<Button>(f,"PauseRecordingButton");var stop=Find<Button>(f,"StopButton");
        Check(enabled.Checked&&!toggle.Enabled,"Recording toggle idle defaults");initial.Checked=true;Check(Convert.ToBoolean(Options(f)["startPaused"]),"Start paused option absent");
        enabled.Checked=false;Check(!initial.Checked&&!initial.Enabled&&!Convert.ToBoolean(Options(f)["recordingControl"]),"Disabled recording control retains initial pause");enabled.Checked=true;
        Invoke(f,"SetBusy",true);typeof(TraceWindow).GetField("captureRequestId",BindingFlags.Instance|BindingFlags.NonPublic).SetValue(f,"control-fixture");
        Action<long,bool,bool,string> state=(generation,paused,pending,name)=>Invoke(f,"HandleMessage",new Dictionary<string,object>{{"type",pending?"recording-pending":"recording-state"},{"id","control-fixture"},{"generation",generation},{"paused",paused},{"pending",pending},{"ready",true},{"state",name},{"recordingActiveMilliseconds",1234}});
        state(0,true,false,"paused");Check(toggle.Enabled&&toggle.Text=="开启采集"&&stop.Enabled&&!initial.Enabled,"Paused capture controls");
        state(1,true,true,"resuming");Check(!toggle.Enabled&&stop.Enabled,"Transition locks only toggle, not stop");
        state(1,false,false,"recording");Check(toggle.Enabled&&toggle.Text=="暂停采集","Resume acknowledgment");
        state(1,true,true,"resuming");Check(toggle.Enabled&&toggle.Text=="暂停采集","Late pending message overwrites settled acknowledgment");
        state(0,true,false,"paused");Check(toggle.Text=="暂停采集","Stale acknowledgment changes toggle");
        toggle.PerformClick();Check(!toggle.Enabled,"Click must await worker acknowledgment");
        var deadline=DateTime.UtcNow.AddSeconds(5);while(!toggle.Enabled&&DateTime.UtcNow<deadline){Application.DoEvents();System.Threading.Thread.Sleep(10);}
        Check(toggle.Enabled&&stop.Enabled&&(bool)typeof(TraceWindow).GetField("busy",BindingFlags.Instance|BindingFlags.NonPublic).GetValue(f),"Control command error ended the capture UI task");
        Invoke(f,"HandleMessage",new Dictionary<string,object>{{"type","stopping"}});state(2,false,false,"recording");Check(!toggle.Enabled&&!stop.Enabled,"Late acknowledgment re-enables control after stop");
        Invoke(f,"SetBusy",false);Check(!toggle.Enabled&&enabled.Enabled&&initial.Enabled,"Idle controls restored");
        var digest=BitConverter.ToString(System.Security.Cryptography.SHA256.Create().ComputeHash(File.ReadAllBytes(typeof(TraceWindow).Assembly.Location))).Replace("-","").ToLowerInvariant();
        Write("ui-recording.json",new{conclusion="PASS",uiSha256=digest,actualCppWorkerControlError=true,startPaused=true,staleAcknowledgmentRejected=true,outOfOrderPendingRejected=true,stopRemainsAvailable=true});
    }
    static void Presets(TraceWindow f) {
        var choice=Find<ComboBox>(f,"PresetInput");var scope=Find<TextBox>(f,"PresetScriptScopeInput");var apply=Find<Button>(f,"ApplyPresetButton");
        Check(choice.Items.Count==22,"21 presets plus custom");
        var payloads=new List<object>();
        Find<TextBox>(f,"UrlInput").Text="https://example.test/";
        Find<TextBox>(f,"OutputInput").Text=@".\logs\catalog-test-new";
        Find<TextBox>(f,"ExportOutputInput").Text=@".\exports\catalog-test-new";
        Find<TextBox>(f,"UntilInput").Text="window.TEST_DONE";
        Find<CheckBox>(f,"HeadlessOption").Checked=true;
        for(int index=1;index<choice.Items.Count;index++) {
            Find<ComboBox>(f,"TransportInput").SelectedIndex=0;
            foreach(var key in rules)Find<TextBox>(f,key+"Input").Text="stale.js";
            Find<CheckBox>(f,"Channel_jsCallDetail").Checked=true;Find<CheckBox>(f,"Channel_nativeDetail").Checked=true;
            Find<CheckBox>(f,"JsCallValuesOption").Checked=true;Find<CheckBox>(f,"DetailFilteringOption").Checked=true;
            Find<NumericUpDown>(f,"WriteBufferInput").Value=16;Find<NumericUpDown>(f,"WriteBatchInput").Value=64;
            var kinds=Find<CheckedListBox>(f,"NativeKindsInput");for(int i=0;i<kinds.Items.Count;i++)kinds.SetItemChecked(i,i==0);
            scope.Text="";var before=SerializeOptions(f);choice.SelectedIndex=index;
            Check(SerializeOptions(f)==before,"Selecting preset mutates options");
            var record=Record(choice.SelectedItem);var expected=(Dictionary<string,object>)record["options"];
            if(record.ContainsKey("available")&&!Convert.ToBoolean(record["available"])) {
                scope.Text="*sample*.js";Check(!apply.Enabled,"Restricted preset enabled with concrete scope");
                Invoke(f,"ApplyPreset");Check(SerializeOptions(f)==before,"Restricted preset changed configuration");
                blockedPresets++;continue;
            }
            bool required=Convert.ToBoolean(record["requiresScriptScope"]);
            if(required) {
                foreach(var invalid in new[]{"","*","target.js\n*"," ?* ? "}) {
                    scope.Text=invalid;Check(!apply.Enabled,"Global/blank scope enabled");
                    Invoke(f,"ApplyPreset");Check(SerializeOptions(f)==before,"Invalid scope partially applied");
                }
            }
            scope.Text="*sample*.js";Check(apply.Enabled,"Concrete scope not enabled");Invoke(f,"ApplyPreset");
            var actual=Options(f);var id=Convert.ToString(record["id"]);
            foreach(var option in expected) {
                if(option.Key=="transport"||option.Key=="nativeKinds"||option.Key=="detailUnknownSource"||option.Value is bool||rules.Contains(option.Key)||numbers.ContainsKey(option.Key)) {
                    Check(actual.ContainsKey(option.Key),id+" missing "+option.Key);
                    var value=required&&option.Key=="detailScriptInclude"?"*sample*.js":Convert.ToString(option.Value);
                    Check(Convert.ToString(actual[option.Key])==value,id+" stale "+option.Key);
                }
            }
            Check(Convert.ToString(actual["preset"])==id,id+" preset identity");
            Check(Find<TextBox>(f,"UrlInput").Text=="https://example.test/"&&Find<TextBox>(f,"OutputInput").Text==@".\logs\catalog-test-new"&&Find<TextBox>(f,"ExportOutputInput").Text==@".\exports\catalog-test-new"&&Find<TextBox>(f,"UntilInput").Text=="window.TEST_DONE"&&Find<CheckBox>(f,"HeadlessOption").Checked,id+" user fields changed");
            Invoke(f,"SetBusy",true);Check(!scope.Enabled&&!apply.Enabled,"Scope controls active while busy");Invoke(f,"SetBusy",false);Check(scope.Enabled&&apply.Enabled,"Scope controls not restored");
            payloads.Add(actual);
            appliedPresets++;
        }
        Check(appliedPresets==(professional?21:17)&&blockedPresets==(professional?0:4),"Exact applied/restricted preset counts");
        Write("ui-options.json",payloads);
        choice.SelectedIndex=4;scope.Text="*sample*.js";Invoke(f,"ApplyPreset");
        Find<CheckBox>(f,"DetailFilteringOption").Checked=false;
        var disabled=Options(f);foreach(var key in rules)Check(Convert.ToString(disabled[key])=="","Disabled filtering retained "+key);
        Check(Convert.ToString(disabled["nativeKinds"])=="all"&&Convert.ToString(disabled["detailUnknownSource"])=="include"&&Convert.ToBoolean(disabled["detailExcludeInternal"])==false,"Disabled filtering reactivates preset defaults");
        Find<CheckBox>(f,"HeadlessOption").Checked=false;
        choice.SelectedIndex=2;Invoke(f,"ApplyPreset");Snapshot(f,"catalog-1280x860.png");
        f.Size=new Size(1000,720);Snapshot(f,"catalog-1000x720.png");f.Size=new Size(1280,860);
    }
    [STAThread] public static int Main(string[] args) {
        var package=args[0];evidence=args[1];temporary=args[2];Directory.SetCurrentDirectory(package);
        Environment.SetEnvironmentVariable("NODE_OPTIONS","--v8trace-must-not-use-node");
        bool skipClipboard=args.Length>3&&args[3]=="no-clipboard";
        IDataObject clipboard=null;bool saved=false;if(!skipClipboard)try{clipboard=RetryClipboard(()=>Clipboard.GetDataObject());saved=true;}catch{}
        TraceWindow f=null;var timer=new Timer{Interval=80};bool entered=false;int stage=0,promptIndex=0;string failure=null;
        var started=DateTime.UtcNow;var longGoal=new string('中',3500)+"😀 保留 {{goal}} 字面量";var rendered=new List<object>();
        try {
            Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
            f=new TraceWindow{ShowInTaskbar=false,StartPosition=FormStartPosition.Manual,Location=new Point(-5000,-5000)};
            timer.Tick+=(sender,eventArgs)=>{
                if(entered)return;entered=true;
                try {
                    Check((DateTime.UtcNow-started).TotalSeconds<90,"UI catalog timeout stage "+stage);
                    if(stage==0) {
                        if(!Find<Button>(f,"StartButton").Enabled)return;
                        Check(Find<Label>(f,"PhaseLabel").Text.Contains("C++"),"Actual C++ worker not ready");
                        professional=(bool)typeof(TraceWindow).GetField("advancedAnalysis",BindingFlags.Instance|BindingFlags.NonPublic).GetValue(f);
                        Presets(f);RecordingControls(f);var templates=Find<ComboBox>(f,"CasePromptChoice");promptCount=professional?6:4;
                        Check(templates.Items.Count==promptCount,"Exact edition template count");
                        foreach(var item in templates.Items) {
                            var id=Convert.ToString(Record(item)["id"]);
                            Check(professional||(id!="jsvmp-evidence"&&id!="wasm-evidence"),"Restricted template offered in Community");
                        }
                        Find<TabControl>(f,"WorkspaceTabs").SelectedTab=Find<TabPage>(f,"CasePromptTab");stage=1;
                    }
                    if(stage==1) {
                        Find<ComboBox>(f,"CasePromptChoice").SelectedIndex=promptIndex;
                        Find<TextBox>(f,"CasePromptGoal").Text=longGoal;
                        Find<TextBox>(f,"CasePromptDirectory").Text="case/中文案例";
                        Invoke(f,"GenerateCasePrompt");stage=2;return;
                    }
                    if(stage==2&&Find<Button>(f,"CopyCasePromptButton").Enabled) {
                        var text=Find<RichTextBox>(f,"CasePromptText").Text;
                        Check(text.Contains(longGoal)&&text.Contains("case/中文案例"),"Long Chinese fields lost");
                        if(!skipClipboard){
                        Check(saved,"Clipboard original state not saved; stop before modifying it");Invoke(f,"CopyCasePrompt");
                        var clipboardText=RetryClipboard(()=>Clipboard.GetText(TextDataFormat.UnicodeText));
                        var mismatch=0;while(mismatch<Math.Min(text.Length,clipboardText.Length)&&text[mismatch]==clipboardText[mismatch])mismatch++;
                        Write("clipboard-probe-"+promptIndex+".json",new{expectedCharacters=text.Length,actualCharacters=clipboardText.Length,firstMismatch=mismatch,expectedNullAt=text.IndexOf('\0'),actualNullAt=clipboardText.IndexOf('\0'),lineEndingsOnly=text.Replace("\r\n","\n")==clipboardText.Replace("\r\n","\n"),copyStatus=Find<Label>(f,"CasePromptStatus").Text});
                        Check(clipboardText==text,"Clipboard mismatch");
                        Check(Find<Label>(f,"CasePromptStatus").Text.Contains("已复制并核验"),"Copy success falsely reported");
                        }
                        var file=Path.Combine(temporary,"prompt-"+promptIndex+".md");Invoke(f,"WriteCasePrompt",file);
                        Check(File.ReadAllText(file,new UTF8Encoding(false,true))==text,"UTF-8 saved prompt mismatch");
                        rendered.Add(new{templateIndex=promptIndex,characters=text.Length,copyVerified=!skipClipboard,utf8Saved=true});
                        if(promptIndex==0)Snapshot(f,"prompt-1280x860.png");
                        promptIndex++;if(promptIndex<promptCount){stage=1;return;}
                        Write("ui-verification.json",new{conclusion="PASS",checks=checks,edition=professional?"professional":"community",presets=21,appliedPresets=appliedPresets,blockedPresets=blockedPresets,promptTemplates=promptCount,clipboardTested=!skipClipboard,rendered=rendered,nodeRequired=false,browserStarted=false});
                        stage=3;timer.Stop();Invoke(f,"Send","shutdown",null);f.Close();
                    }
                }catch(Exception error){failure=error.ToString();timer.Stop();try{Invoke(f,"Send","shutdown",null);}catch{}f.Close();}
                finally{entered=false;}
            };
            timer.Start();Application.Run(f);Check(failure==null,failure);Check(stage==3,"UI closed early");return 0;
        }catch(Exception error){Write("ui-verification.json",new{conclusion="FAIL",checks=checks,error=error.ToString()});return 1;}
        finally{timer.Dispose();if(f!=null)f.Dispose();if(saved){try{if(clipboard==null)Clipboard.Clear();else Clipboard.SetDataObject(clipboard,true);}catch{}}}
    }
}
'@
try {
    $sourceFile = Join-Path $temporary 'catalog-smoke.cs'
    $exe = Join-Path $temporary 'catalog-smoke.exe'
    [IO.File]::WriteAllText($sourceFile,$source,[Text.UTF8Encoding]::new($false))
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $compiler /nologo /target:exe /platform:x64 /codepage:65001 ('/out:'+$exe) /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll ('/reference:'+(Join-Path $package 'V8Trace.exe')) $sourceFile
    if ($LASTEXITCODE -ne 0) { throw 'UI catalog compiler failed' }
    Copy-Item -LiteralPath (Join-Path $package 'V8Trace.exe') -Destination $temporary
    Copy-Item -LiteralPath (Join-Path $package 'runtime') -Destination (Join-Path $temporary 'runtime') -Recurse
    Copy-Item -LiteralPath (Join-Path $package 'app') -Destination (Join-Path $temporary 'app') -Recurse
    $clipboardMode=if($SkipClipboard){'no-clipboard'}else{'clipboard'}
    & $exe $package $evidence $temporary $clipboardMode
    if ($LASTEXITCODE -ne 0) { throw 'UI catalog regression failed; see ui-verification.json' }
    Write-Output ('PASS actual C++ UI: exact edition preset availability and all offered long Chinese prompts; clipboard tested: '+(-not $SkipClipboard))
} finally {
    $resolved = [IO.Path]::GetFullPath($temporary)
    $parent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if ((Split-Path -Parent $resolved) -ne $parent -or (Split-Path -Leaf $resolved) -notmatch '^v8trace-catalog-ui-[a-f0-9]{32}$') { throw 'Temporary cleanup boundary mismatch' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
    [IO.File]::WriteAllText((Join-Path $evidence 'cleanup.json'),'{"temporaryFixturesRemoved":true}',[Text.UTF8Encoding]::new($false))
}
