[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$PackagePath,
    [Parameter(Mandatory=$true)][string]$EvidencePath,
    [Parameter(Mandatory=$true)][ValidateSet('community','professional')][string]$Edition
)
$ErrorActionPreference='Stop'
$package=(Resolve-Path -LiteralPath $PackagePath).Path
$evidence=[IO.Path]::GetFullPath($EvidencePath)
if(Test-Path -LiteralPath $evidence){throw 'A fresh sharing-copy test directory is required'}
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('v8trace-share-ui-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary -Force | Out-Null
$exe=Join-Path $package ('ShareCopyTests-'+[guid]::NewGuid().ToString('N')+'.exe')
try {
    & "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:exe /platform:x64 /codepage:65001 ('/out:'+$exe) /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll ('/reference:'+(Join-Path $package 'V8Trace.exe')) (Join-Path $PSScriptRoot 'ui_share_copy_tests.cs')
    if($LASTEXITCODE -ne 0){throw 'Sharing-copy UI test compilation failed'}
    & $exe $evidence $temporary $Edition
    if($LASTEXITCODE -ne 0){throw 'Sharing-copy UI test failed; see result.json'}
} finally {
    if((Split-Path -Parent $exe) -ne $package -or (Split-Path -Leaf $exe) -notmatch '^ShareCopyTests-[a-f0-9]{32}\.exe$'){throw 'Test executable cleanup boundary mismatch'}
    if(Test-Path -LiteralPath $exe){Remove-Item -LiteralPath $exe -Force}
    $resolved=[IO.Path]::GetFullPath($temporary)
    $parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if((Split-Path -Parent $resolved) -ne $parent -or (Split-Path -Leaf $resolved) -notmatch '^v8trace-share-ui-[a-f0-9]{32}$'){throw 'Sharing-copy fixture cleanup boundary mismatch'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
