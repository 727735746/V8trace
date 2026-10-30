[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackagePath,[Parameter(Mandatory=$true)][string]$EvidencePath)
$ErrorActionPreference='Stop'
$package=(Resolve-Path -LiteralPath $PackagePath).Path
$evidence=[IO.Path]::GetFullPath($EvidencePath)
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('v8trace-storage-theme-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary -Force | Out-Null
try {
    $target=Join-Path $temporary 'junction-target';New-Item -ItemType Directory -Path $target -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $target 'keep.txt'),'owned junction fixture')
    $managed=Join-Path $temporary 'managed';New-Item -ItemType Directory -Path $managed -Force | Out-Null
    $link=Join-Path $managed 'junction';New-Item -ItemType Junction -Path $link -Value $target | Out-Null
    $exe=Join-Path $temporary 'StorageThemeTests.exe'
    & "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:exe /platform:x64 /codepage:65001 ('/out:'+$exe) /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll ('/reference:'+(Join-Path $package 'V8Trace.exe')) (Join-Path $PSScriptRoot 'ui_storage_theme_tests.cs')
    if($LASTEXITCODE -ne 0){throw 'Storage/theme test compilation failed'}
    Copy-Item -LiteralPath (Join-Path $package 'V8Trace.exe') -Destination $temporary
    & $exe $temporary (Join-Path $evidence 'result.json')
    if($LASTEXITCODE -ne 0){throw 'Storage/theme test failed; see result.json'}
    if(-not(Test-Path -LiteralPath (Join-Path $target 'keep.txt'))){throw 'Junction target was changed'}
} finally {
    $resolved=[IO.Path]::GetFullPath($temporary);$parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if((Split-Path -Parent $resolved) -ne $parent -or (Split-Path -Leaf $resolved) -notmatch '^v8trace-storage-theme-[a-f0-9]{32}$'){throw 'Test cleanup boundary mismatch'}
    if(Test-Path -LiteralPath (Join-Path $resolved 'managed\junction')){Remove-Item -LiteralPath (Join-Path $resolved 'managed\junction') -Force}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
