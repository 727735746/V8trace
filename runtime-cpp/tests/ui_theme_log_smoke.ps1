[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackagePath,[Parameter(Mandatory=$true)][string]$EvidencePath,[Parameter(Mandatory=$true)][ValidateSet('community','professional')][string]$Edition)
if($PSVersionTable.PSEdition -ne 'Desktop') { & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -STA -File $PSCommandPath -PackagePath $PackagePath -EvidencePath $EvidencePath -Edition $Edition; exit $LASTEXITCODE }
$ErrorActionPreference='Stop'
$package=(Resolve-Path -LiteralPath $PackagePath).Path;$evidence=[IO.Path]::GetFullPath($EvidencePath)
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('v8trace-theme-log-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    $exe=Join-Path $temporary 'ThemeLogSmoke.exe'
    & "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:exe /platform:x64 /codepage:65001 ('/out:'+$exe) /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll ('/reference:'+(Join-Path $package 'V8Trace.exe')) (Join-Path $PSScriptRoot 'ui_theme_log_smoke.cs')
    if($LASTEXITCODE -ne 0){throw 'Theme/log UI harness compilation failed'}
    Copy-Item -LiteralPath (Join-Path $package 'V8Trace.exe') -Destination $temporary
    foreach($name in @('runtime','app')){Copy-Item -LiteralPath (Join-Path $package $name) -Destination (Join-Path $temporary $name) -Recurse}
    $fixture=Join-Path $temporary 'fixtures';New-Item -ItemType Directory -Path $fixture | Out-Null
    & $exe $evidence $fixture $Edition
    if($LASTEXITCODE -ne 0){throw 'Theme/log actual UI smoke failed; see result.json'}
} finally {
    $resolved=[IO.Path]::GetFullPath($temporary);$parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if((Split-Path -Parent $resolved) -ne $parent -or (Split-Path -Leaf $resolved) -notmatch '^v8trace-theme-log-[a-f0-9]{32}$'){throw 'UI fixture cleanup boundary mismatch'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
