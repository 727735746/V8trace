[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$PackagePath,
    [Parameter(Mandatory=$true)][string]$EvidencePath,
    [Parameter(Mandatory=$true)][ValidateSet('community','professional')][string]$Edition,
    [switch]$RequireDrainedExit
)
$ErrorActionPreference='Stop'
$package=(Resolve-Path -LiteralPath $PackagePath).Path
$evidence=[IO.Path]::GetFullPath($EvidencePath)
if(Test-Path -LiteralPath $evidence){throw 'A fresh benchmark evidence directory is required'}
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('v8trace-queue-bench-'+[guid]::NewGuid().ToString('N'))
$fixture=Join-Path $temporary 'package'
New-Item -ItemType Directory -Path (Join-Path $fixture 'runtime') -Force | Out-Null
try {
    foreach($name in @('V8Trace.exe','V8Trace.exe.config')){Copy-Item -LiteralPath (Join-Path $package $name) -Destination $fixture}
    $runtime=Join-Path $fixture 'runtime'
    & python -B (Join-Path $PSScriptRoot 'ui_queue_ready.py') (Join-Path $package 'runtime\v8trace-runtime.exe') (Join-Path $package 'app\v8-trace') (Join-Path $runtime 'fixture-ready.json') $Edition
    if($LASTEXITCODE -ne 0){throw 'Actual C++ ready fixture intake failed'}
    Copy-Item -LiteralPath (Join-Path $runtime 'fixture-ready.json') -Destination (Join-Path $evidence 'actual-cpp-ready.json')
    $compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    $references=@('/reference:System.dll','/reference:System.Core.dll','/reference:System.Drawing.dll','/reference:System.Windows.Forms.dll','/reference:System.Web.Extensions.dll')
    & $compiler /nologo /target:exe /platform:x64 /codepage:65001 ('/out:'+(Join-Path $runtime 'v8trace-runtime.exe')) @references (Join-Path $PSScriptRoot 'ui_queue_fixture.cs')
    if($LASTEXITCODE -ne 0){throw 'Owned wire fixture compilation failed'}
    & $compiler /nologo /target:exe /platform:x64 /codepage:65001 ('/out:'+(Join-Path $fixture 'QueueBenchmark.exe')) @references ('/reference:'+(Join-Path $fixture 'V8Trace.exe')) (Join-Path $PSScriptRoot 'ui_queue_benchmark.cs')
    if($LASTEXITCODE -ne 0){throw 'UI queue benchmark compilation failed'}
    $mode=if($RequireDrainedExit){'drained'}else{'baseline'}
    & (Join-Path $fixture 'QueueBenchmark.exe') $evidence $fixture $Edition $mode
    if($LASTEXITCODE -ne 0){throw 'UI queue benchmark failed; see result.json'}
} finally {
    $wire=Join-Path $fixture 'runtime\fixture-wire.jsonl'
    if((Test-Path -LiteralPath $wire) -and -not(Test-Path -LiteralPath (Join-Path $evidence 'fixture-wire.jsonl'))){Copy-Item -LiteralPath $wire -Destination (Join-Path $evidence 'fixture-wire.jsonl')}
    $resolved=[IO.Path]::GetFullPath($temporary)
    $parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if((Split-Path -Parent $resolved) -ne $parent -or (Split-Path -Leaf $resolved) -notmatch '^v8trace-queue-bench-[a-f0-9]{32}$'){throw 'Benchmark cleanup boundary mismatch'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
