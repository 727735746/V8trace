[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$PackagePath,
    [Parameter(Mandatory=$true)][string]$EvidencePath,
    [Parameter(Mandatory=$true)][string]$DescriptorPath,
    [Parameter(Mandatory=$true)][ValidateSet('community','professional')][string]$Edition
)
$ErrorActionPreference='Stop'
$package=(Resolve-Path -LiteralPath $PackagePath).Path
$evidence=[IO.Path]::GetFullPath($EvidencePath)
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('v8trace-scenario-ui-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary -Force | Out-Null
try {
    $exe=Join-Path $package ('ScenarioContractTests-'+[guid]::NewGuid().ToString('N')+'.exe')
    & "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:exe /platform:x64 /codepage:65001 ('/out:'+$exe) /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll ('/reference:'+(Join-Path $package 'V8Trace.exe')) (Join-Path $PSScriptRoot 'ui_scenario_contract_tests.cs')
    if($LASTEXITCODE -ne 0){throw 'Scenario UI test compilation failed'}
    & $exe $package $evidence $temporary $Edition (Resolve-Path -LiteralPath $DescriptorPath).Path
    if($LASTEXITCODE -ne 0){throw 'Scenario UI test failed; see result.json'}
} finally {
    if($exe -and (Split-Path -Parent $exe) -eq $package -and (Split-Path -Leaf $exe) -match '^ScenarioContractTests-[a-f0-9]{32}\.exe$' -and (Test-Path -LiteralPath $exe)){Remove-Item -LiteralPath $exe -Force}
    $resolved=[IO.Path]::GetFullPath($temporary)
    $parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if((Split-Path -Parent $resolved) -ne $parent -or (Split-Path -Leaf $resolved) -notmatch '^v8trace-scenario-ui-[a-f0-9]{32}$'){throw 'Test cleanup boundary mismatch'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
