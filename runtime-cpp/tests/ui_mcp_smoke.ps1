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
$temporary=Join-Path ([IO.Path]::GetFullPath((Join-Path $evidence '..'))) ('v8trace-mcp-ui-test-'+[guid]::NewGuid().ToString('N')+' 中文 空格')
New-Item -ItemType Directory -Path $temporary|Out-Null
$source=@'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using V8Trace;
public static class McpUiSmoke {
    static readonly JavaScriptSerializer json=new JavaScriptSerializer();
    static string root,evidence;static int checks;static Form form;static TcpListener occupied;
    static void Check(bool ok,string message){++checks;if(!ok)throw new Exception(message);}
    static T Find<T>(string name)where T:Control {var controls=form.Controls.Find(name,true);Check(controls.Length==1,"Missing "+name);return (T)controls[0];}
    static object Field(string name){return typeof(TraceWindow).GetField(name,BindingFlags.NonPublic|BindingFlags.Instance).GetValue(form);}
    static object Invoke(string name,params object[] args){return typeof(TraceWindow).GetMethod(name,BindingFlags.NonPublic|BindingFlags.Instance).Invoke(form,args);}
    static void Write(string file,object value){File.WriteAllText(Path.Combine(evidence,file),json.Serialize(value),new UTF8Encoding(false));}
    static int Port(){var listener=new TcpListener(IPAddress.Loopback,0);listener.Start();int port=((IPEndPoint)listener.LocalEndpoint).Port;listener.Stop();return port;}
    static Dictionary<string,object> Health(int port,string token){var request=(HttpWebRequest)WebRequest.Create("http://127.0.0.1:"+port+"/healthz");request.Proxy=null;request.Headers["Authorization"]="Bearer "+token;request.Timeout=2000;using(var response=request.GetResponse())using(var reader=new StreamReader(response.GetResponseStream(),Encoding.UTF8))return json.Deserialize<Dictionary<string,object>>(reader.ReadToEnd());}
    [DllImport("user32.dll")]static extern bool PrintWindow(IntPtr window,IntPtr dc,uint flags);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern uint GetFileAttributesW(string path);
    static string Physical(string path){path=Path.GetFullPath(path);return path.StartsWith(@"\\?\")?path:path.StartsWith(@"\\")?@"\\?\UNC\"+path.Substring(2):@"\\?\"+path;}
    static void Snapshot(string name){using(var bitmap=new Bitmap(form.Width,form.Height))using(var graphics=Graphics.FromImage(bitmap)){var dc=graphics.GetHdc();try{Check(PrintWindow(form.Handle,dc,2),"PrintWindow");}finally{graphics.ReleaseHdc(dc);}bitmap.Save(Path.Combine(evidence,name),System.Drawing.Imaging.ImageFormat.Png);}}
    static void Visible(Control control){var original=control.RectangleToScreen(control.ClientRectangle);var visible=original;for(var parent=control.Parent;parent!=null;parent=parent.Parent)visible=Rectangle.Intersect(visible,parent.RectangleToScreen(parent.ClientRectangle));Check(visible.Width>=original.Width-1&&visible.Height>=original.Height-1,"Clipped "+control.Name+" "+original+" => "+visible);}
    [STAThread]public static int Main(string[] args){
        root=args[0];evidence=args[1];int stage=0,port=Port();string token=null,failure=null;bool entered=false;var timer=new Timer{Interval=80};var start=DateTime.UtcNow;
        try{
            Directory.CreateDirectory(Path.Combine(root,"logs"));File.WriteAllText(Path.Combine(root,"logs","sample.log"),"alpha\nbeta\n",new UTF8Encoding(false));
            form=new TraceWindow();form.StartPosition=FormStartPosition.Manual;form.Location=new Point(-3000,0);form.ShowInTaskbar=false;
            timer.Tick+=(sender,e)=>{if(entered)return;entered=true;try{
                Check((DateTime.UtcNow-start).TotalSeconds<45,"UI deadline; status="+Find<Label>("McpStatus").Text);
                var service=(McpService)Field("mcpService");
                if(stage==0&&(bool)Field("ready")){
                    Find<TabControl>("WorkspaceTabs").SelectedTab=Find<TabPage>("McpTab");Check(!service.Active,"MCP must not auto-start");Check(Find<TextBox>("McpSourceRoot").Text==@".\logs","Portable source default");Find<NumericUpDown>("McpPort").Value=port;
                    string index=Path.Combine(root,"mcp","index");while(index.Length<420)index=Path.Combine(index,"中文 深层索引原路径-0123456789");Find<TextBox>("McpIndexRoot").Text=index;
                    var configuration=(Dictionary<string,object>)Invoke("McpClientConfiguration","stdio");var servers=(Dictionary<string,object>)configuration["mcpServers"];var item=(Dictionary<string,object>)servers["v8trace"];Check((string)item["command"]==Path.Combine(root,"runtime","v8trace-runtime.exe"),"Stdio command at current package location");Check(((string[])item["args"])[4]==Path.Combine(root,"logs"),"Relative source rooted at software directory");Check(((string[])item["args"])[6]==index+"-stdio","UI keeps full ordinary long index path");
                    Snapshot("mcp-idle-1280.png");Find<Button>("McpStartButton").PerformClick();Check(service.Active,"MCP process started independently");stage=1;
                }else if(stage==1&&(bool)Field("mcpReady")){
                    token=File.ReadAllText(Path.Combine(root,"mcp","token.txt")).Trim();Check(token.Length==64,"Random token generated");var security=File.GetAccessControl(Path.Combine(root,"mcp","token.txt"));Check(security.AreAccessRulesProtected,"Token ACL protected");var health=Health(port,token);Check((string)health["runtime"]=="cpp"&&Convert.ToInt32(health["tools"])==11,"Real native service health");
                    Check((string)health["indexRoot"]==Find<TextBox>("McpIndexRoot").Text.Replace('\\','/'),"Backend keeps requested long cache root");var attributes=GetFileAttributesW(Physical(Find<TextBox>("McpIndexRoot").Text));Check(attributes!=UInt32.MaxValue&&(attributes&16)!=0,"Actual native long cache directory exists");
                    Check(!Find<TextBox>("McpSourceRoot").Enabled&&!Find<Button>("McpStartButton").Enabled&&Find<Button>("McpStopButton").Enabled,"Running UI controls");Invoke("SetBusy",true);Check(service.Active&&Find<Button>("McpStopButton").Enabled,"Capture busy state does not own MCP process");Invoke("SetBusy",false);
                    Snapshot("mcp-ready-1280.png");form.Size=new Size(1000,720);form.PerformLayout();Application.DoEvents();foreach(var name in new[]{"McpSourceRoot","McpIndexRoot","McpExportRoot","McpTokenFile","McpPort","McpStartButton","McpStopButton","McpClientConfiguration"})Visible(Find<Control>(name));Check(Find<RichTextBox>("McpClientConfiguration").Height>=45,"MCP config pane usable at minimum size");Snapshot("mcp-ready-1000.png");Find<Button>("McpStopButton").PerformClick();stage=2;
                }else if(stage==2&&!service.Active){Check(Find<Label>("McpStatus").Text.Contains("已停止"),"Graceful UI stop receipt");Find<Button>("McpStartButton").PerformClick();stage=3;}
                else if(stage==3){
                    if(!service.Active)throw new Exception("Same-port restart failed: "+Find<RichTextBox>("McpClientConfiguration").Text);
                    if((bool)Field("mcpReady")){Check(Health(port,token).ContainsKey("tools"),"Immediate restart same port");Find<Button>("McpStopButton").PerformClick();stage=4;}
                }else if(stage==4&&!service.Active){
                    occupied=new TcpListener(IPAddress.Loopback,0);occupied.ExclusiveAddressUse=true;occupied.Start();Find<NumericUpDown>("McpPort").Value=((IPEndPoint)occupied.LocalEndpoint).Port;Find<Button>("McpStartButton").PerformClick();stage=5;
                }else if(stage==5&&!service.Active){
                    Check(occupied.Server.IsBound,"Port conflict did not stop unrelated listener");Check(Find<RichTextBox>("McpClientConfiguration").Text.Contains("bind")||Find<RichTextBox>("McpClientConfiguration").Text.Contains("port"),"Port conflict shown");occupied.Stop();occupied=null;Find<NumericUpDown>("McpPort").Value=Port();Find<Button>("McpStartButton").PerformClick();stage=6;
                }else if(stage==6&&(bool)Field("mcpReady")){Write("ui-mcp.json",new{conclusion="PASS",checks=checks,cppService=true,portablePaths=true,longIndexPathChars=Find<TextBox>("McpIndexRoot").Text.Length,tokenAcl=true,immediateRestart=true,portConflict=true,browserStarted=false,clipboardUnchanged=true});stage=7;timer.Stop();form.Close();}
            }catch(Exception error){failure=error.ToString();timer.Stop();try{Invoke("SetBusy",false);}catch{}form.Close();}finally{entered=false;}};
            timer.Start();Application.Run(form);Check(failure==null,failure);Check(stage==7,"UI ended before acceptance");Check(!((McpService)Field("mcpService")).Active,"Closing UI stopped owned MCP");return 0;
        }catch(Exception error){Write("ui-mcp.json",new{conclusion="FAIL",checks=checks,stage=stage,error=error.ToString()});return 1;}
        finally{timer.Dispose();if(occupied!=null)occupied.Stop();if(form!=null)form.Dispose();var owned=Path.GetFullPath(Path.Combine(root,"mcp","index"));if(!owned.StartsWith(Path.GetFullPath(root).TrimEnd('\\')+"\\",StringComparison.OrdinalIgnoreCase))throw new Exception("Owned long index cleanup boundary changed");if(Directory.Exists(Physical(owned)))Directory.Delete(Physical(owned),true);}
    }
}
'@
try {
    $sourceFile=Join-Path $temporary 'mcp-ui-smoke.cs'
    $exe=Join-Path $temporary 'mcp-ui-smoke.exe'
    [IO.File]::WriteAllText($sourceFile,$source,[Text.UTF8Encoding]::new($false))
    & "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:exe /platform:x64 /codepage:65001 ('/out:'+$exe) /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll ('/reference:'+(Join-Path $package 'V8Trace.exe')) $sourceFile
    if($LASTEXITCODE -ne 0){throw 'MCP UI test compile failed'}
    Copy-Item -LiteralPath (Join-Path $package 'V8Trace.exe') -Destination $temporary
    Copy-Item -LiteralPath (Join-Path $package 'V8Trace.exe.config') -Destination ($exe+'.config')
    Copy-Item -LiteralPath (Join-Path $package 'runtime') -Destination (Join-Path $temporary 'runtime') -Recurse
    Copy-Item -LiteralPath (Join-Path $package 'app') -Destination (Join-Path $temporary 'app') -Recurse
    & $exe $temporary $evidence
    if($LASTEXITCODE -ne 0){throw 'MCP UI smoke failed; see ui-mcp.json'}
    Write-Output 'PASS native MCP UI lifecycle and configuration'
} finally {
    $resolved=[IO.Path]::GetFullPath($temporary)
    if((Split-Path -Parent $resolved) -ne [IO.Path]::GetFullPath((Join-Path $evidence '..')).TrimEnd('\') -or (Split-Path -Leaf $resolved) -notmatch '^v8trace-mcp-ui-test-[a-f0-9]{32} 中文 空格$'){throw 'Owned MCP UI cleanup boundary mismatch'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
    [IO.File]::WriteAllText((Join-Path $evidence 'cleanup.json'),'{"temporaryFixturesRemoved":true}',[Text.UTF8Encoding]::new($false))
}
