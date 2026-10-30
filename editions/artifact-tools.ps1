if($PSVersionTable.PSEdition -eq 'Desktop') {
    $env:PSModulePath=(Join-Path $PSHOME 'Modules')+[IO.Path]::PathSeparator+$env:PSModulePath
    Import-Module -Name (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
}
$script:V8TraceBuildToolRoot=$PSScriptRoot
function Invoke-V8TraceBuildTool {
    param([ValidateSet('artifact_binding','workspace_plan')][string]$Tool,[string[]]$Arguments)
    & python -B (Join-Path $script:V8TraceBuildToolRoot ($Tool+'.py')) @Arguments
    if($LASTEXITCODE -ne 0){throw ($Tool+' verification failed: '+$LASTEXITCODE)}
}
function Open-V8TraceHostLease {
    param([Parameter(Mandatory=$true)][string]$Root)
    if(-not ('V8Trace.BuildLease' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace V8Trace {
    public sealed class BuildLease : IDisposable {
        [StructLayout(LayoutKind.Sequential)] struct Info { public uint Attributes, CreatedLow, CreatedHigh, AccessLow, AccessHigh, WriteLow, WriteHigh, Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow; }
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern SafeFileHandle CreateFile(string name,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetFileInformationByHandle(SafeFileHandle file,out Info info);
        readonly List<SafeFileHandle> handles=new List<SafeFileHandle>();
        public BuildLease(string root) {
            try {
                root=Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar,Path.AltDirectorySeparatorChar);var directories=new List<string>();for(var directory=new DirectoryInfo(root);directory!=null;directory=directory.Parent)directories.Add(directory.FullName);directories.Reverse();
                foreach(var directory in directories) {
                    var handle=CreateFile(directory,0x80,directory==root?1u:3u,IntPtr.Zero,3,0x02000000|0x00200000,IntPtr.Zero);handles.Add(handle);Info info;
                    if(handle.IsInvalid||!GetFileInformationByHandle(handle,out info)||(info.Attributes&0x400)!=0||(info.Attributes&0x10)==0)throw new IOException("Host workspace reparse/ownership guard failed");
                }
                var lease=CreateFile(Path.Combine(root,".v8trace-host.lock"),0x80000000u|0x40000000u,0,IntPtr.Zero,4,0x00200000,IntPtr.Zero);handles.Add(lease);Info selected;
                if(lease.IsInvalid||!GetFileInformationByHandle(lease,out selected)||(selected.Attributes&0x410)!=0||selected.Links!=1)throw new IOException("Host workspace lease is active, aliased or not ordinary");
            } catch {Dispose();throw;}
        }
        public void Dispose(){for(int i=handles.Count-1;i>=0;--i)handles[i].Dispose();handles.Clear();}
    }
}
'@
    }
    return [V8Trace.BuildLease]::new([IO.Path]::GetFullPath($Root))
}
