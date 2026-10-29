[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BuildRoot,
    [string]$DependencyRoot,
    [string]$MsvcEnvironment,
    [ValidateSet('community','professional')][string[]]$Editions = @('community','professional'),
    [ValidateRange(1,4)][int]$Jobs = 1
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'runtime-identity.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build=[IO.Path]::GetFullPath($BuildRoot)
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    if (-not $MsvcEnvironment) {
        $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { throw '请从 x64 Native Tools 命令行运行或指定 -MsvcEnvironment。' }
        $installation=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'MSVC x64 工具链未找到。' }
        $MsvcEnvironment=Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
    }
    if (-not (Test-Path -LiteralPath $MsvcEnvironment -PathType Leaf)) { throw 'MSVC 环境脚本不存在。' }
    $psi=[Diagnostics.ProcessStartInfo]::new();$psi.FileName=$env:ComSpec;$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $psi.Arguments='/d /s /c ""'+$MsvcEnvironment+'" >nul && set"'
    $process=[Diagnostics.Process]::Start($psi);$environment=$process.StandardOutput.ReadToEnd();$errorText=$process.StandardError.ReadToEnd();$process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw ('MSVC 环境加载失败：'+$errorText) }
    foreach($line in $environment.Split("`n")){if($line.Contains('=')){$pair=$line.TrimEnd("`r").Split('=',2);if($pair[0] -in @('INCLUDE','LIB','LIBPATH','PATH','WindowsSDKDir','WindowsSDKVersion','VCToolsInstallDir','VSINSTALLDIR','UniversalCRTSdkDir','UCRTVersion')){[Environment]::SetEnvironmentVariable($pair[0],$pair[1],'Process')}}}
}
$env:PYTHONDONTWRITEBYTECODE='1';$env:PYTHONUTF8='1';$env:VSLANG='1033'
$encoding=[Text.UTF8Encoding]::new($false)
New-Item -ItemType Directory -Path $build -Force | Out-Null
foreach($edition in $Editions) {
    $edition=$edition.ToLowerInvariant();$directory=Join-Path $build $edition
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $attempt=Join-Path $directory ('verification-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $attempt | Out-Null
    $args=@('-S',(Join-Path $root 'runtime-cpp'),'-B',$directory,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',('-DV8TRACE_EDITION='+$edition))
    if($DependencyRoot){foreach($name in @('json','zlib','brotli','zstd','ixwebsocket','sqlite','abseil','re2')){$source=[IO.Path]::GetFullPath((Join-Path $DependencyRoot ('_deps\'+$name+'-src')));if(-not(Test-Path -LiteralPath $source -PathType Container)){throw ('缺少可复用依赖源：'+$source)};$args+=('-DFETCHCONTENT_SOURCE_DIR_'+$name.ToUpperInvariant()+'='+$source)}}
    & cmake @args *> (Join-Path $attempt 'configure.log')
    if($LASTEXITCODE -ne 0){throw ('配置失败，见 '+$attempt+'\configure.log')}
    & cmake --build $directory --parallel $Jobs *> (Join-Path $attempt 'build.log')
    if($LASTEXITCODE -ne 0){throw ('编译失败，见 '+$attempt+'\build.log')}
    $report=Join-Path $attempt 'ctest.xml'
    & ctest --test-dir $directory --output-on-failure --output-junit $report *> (Join-Path $attempt 'ctest.log')
    if($LASTEXITCODE -ne 0){throw ('测试失败，见 '+$attempt+'\ctest.log')}
    [xml]$tests=Get-Content -LiteralPath $report -Raw -Encoding UTF8
    if([int]$tests.testsuite.tests -lt 1 -or [int]$tests.testsuite.failures -ne 0 -or [int]$tests.testsuite.disabled -ne 0){throw 'CTest 门槛未全部执行通过。'}
    $runtime=Join-Path $directory 'v8trace-runtime.exe'
    $identity=Get-V8TraceRuntimeIdentity -Runtime $runtime
    if($identity.edition.id -ne $edition -or $identity.edition.protocol -ne 'v8trace/edition/1'){throw '实际内核身份不匹配。'}
    [IO.File]::WriteAllText((Join-Path $attempt 'runtime-version.json'),($identity | ConvertTo-Json -Depth 6),$encoding)
    $receipt=[ordered]@{protocol='v8trace/edition-verification/1';edition=$edition;status='host-tests-passed';testCount=[int]$tests.testsuite.tests;testReport=$report;runtime=$runtime;runtimeSha256=(Get-FileHash -LiteralPath $runtime).Hash.ToLowerInvariant();nativeBrowserValidated=$false;build=$directory;finishedAt=[DateTimeOffset]::UtcNow.ToString('o')}
    [IO.File]::WriteAllText((Join-Path $directory 'verification.json'),($receipt | ConvertTo-Json -Depth 6),$encoding)
    [IO.File]::WriteAllText((Join-Path $attempt 'verification.json'),($receipt | ConvertTo-Json -Depth 6),$encoding)
    [pscustomobject]$receipt | ConvertTo-Json -Depth 6
}
