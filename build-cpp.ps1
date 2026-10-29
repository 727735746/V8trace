[CmdletBinding()]
param(
    [string]$OutputPath,
    [string]$BuildRoot,
    [string]$NativeChromePath,
    [ValidateSet('community','professional')][string]$Edition,
    [ValidateRange(1, 32)][int]$Jobs = 4
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'editions\runtime-identity.ps1')
if (-not $Edition) { $Edition = if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'professional\runtime\sources.cmake')) { 'professional' } else { 'community' } }
$Edition = $Edition.ToLowerInvariant()
if (-not $OutputPath) { $OutputPath = Join-Path $PSScriptRoot ('dist\V8Trace-' + $Edition) }
if (-not $BuildRoot) { $BuildRoot = Join-Path $PSScriptRoot ('.cache\cpp-' + $Edition) }
$output = [System.IO.Path]::GetFullPath($OutputPath)
$build = [System.IO.Path]::GetFullPath($BuildRoot)
if (Test-Path -LiteralPath $output) { throw "输出目录已存在，请选择新目录：$output" }
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'Windows UI 需要 .NET Framework C# 编译器；采集内核为独立 C++ 程序。' }
& cmake -S (Join-Path $PSScriptRoot 'runtime-cpp') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release ('-DV8TRACE_EDITION=' + $Edition)
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败：$LASTEXITCODE" }
& cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "C++ 编译失败：$LASTEXITCODE" }
& ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "C++ 测试失败：$LASTEXITCODE" }
$runtime = Join-Path $output 'runtime'
$assets = Join-Path $output 'app\v8-trace'
New-Item -ItemType Directory -Path $runtime,$assets -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'v8trace-runtime.exe') -Destination $runtime
$identity = Get-V8TraceRuntimeIdentity -Runtime (Join-Path $runtime 'v8trace-runtime.exe')
if ($identity.edition.protocol -ne 'v8trace/edition/1' -or $identity.edition.id -ne $Edition -or -not $identity.edition.buildTimeIdentity) { throw '构建内核版本与打包请求不一致' }
foreach ($name in @('scenario_presets.json','case_prompt_templates.json')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $assets }
Copy-Item -LiteralPath (Join-Path $build 'diagnostic-hooks.js') -Destination $assets
$encoding = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText((Join-Path $runtime 'runtime.json'), '{"engine":"cpp","automaticFallback":false}', $encoding)
[System.IO.File]::WriteAllText((Join-Path $runtime 'edition.json'), ($identity.edition | ConvertTo-Json -Depth 6), $encoding)
$arguments = @('/nologo','/target:winexe','/platform:x64','/optimize+','/codepage:65001',('/out:' + (Join-Path $output 'V8Trace.exe')),('/win32manifest:' + (Join-Path $PSScriptRoot 'ui\V8Trace.exe.manifest')),'/reference:System.dll','/reference:System.Core.dll','/reference:System.Drawing.dll','/reference:System.Windows.Forms.dll','/reference:System.Web.Extensions.dll')
foreach ($name in @('TraceWindow.cs','TraceWindow.Studio.cs','TraceWindow.Causality.cs','TraceWindow.Edition.cs','TraceWindow.Mcp.cs','McpService.cs','WorkerClient.cs','AssemblyInfo.cs')) { $arguments += Join-Path $PSScriptRoot ('ui\' + $name) }
& $compiler @arguments
if ($LASTEXITCODE -ne 0) { throw "UI 编译失败：$LASTEXITCODE" }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'ui\V8Trace.exe.config') -Destination $output
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'runtime-cpp\README.md') -Destination (Join-Path $output 'README.md')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native\JSCall优化说明.md') -Destination (Join-Path $output 'JSCall优化说明.md')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'runtime-cpp\JSVMP与缓冲说明.md') -Destination (Join-Path $output 'JSVMP与缓冲说明.md')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'runtime-cpp\大日志索引说明.md') -Destination (Join-Path $output '大日志索引说明.md')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'runtime-cpp\原生环境取证说明.md') -Destination (Join-Path $output '原生环境取证说明.md')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'runtime-cpp\闭包快照说明.md') -Destination (Join-Path $output '闭包快照说明.md')
foreach ($name in @('网络状态专项说明.md','持续分块说明.md','VM探针与WASM模块说明.md','配置与证据索引说明.md','HTTP首栈与参数DOM插桩说明.md','后台HTTP与常驻Job完整性说明.md')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('runtime-cpp\'+$name)) -Destination (Join-Path $output $name) }
foreach ($name in @('MCP搜索分析说明.md','trace-search来源与适配.md','mcp-client.example.json')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('runtime-cpp\'+$name)) -Destination (Join-Path $output $name) }
foreach ($name in @('README-cheatsheet.md','操作手册.md','编译指令.txt')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $output $name) }
$licenses = Join-Path $output 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'editions\community\LICENSE') -Destination (Join-Path $licenses 'v8trace-community-Apache-2.0.txt')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'editions\community\NOTICE') -Destination (Join-Path $licenses 'v8trace-community-NOTICE.txt')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'editions\版本拆分说明.md') -Destination (Join-Path $output '版本拆分说明.md')
if ($Edition -eq 'community') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'editions\community\LICENSE') -Destination (Join-Path $output 'LICENSE')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'editions\community\NOTICE') -Destination (Join-Path $output 'NOTICE')
} else {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'professional\NOTICE') -Destination (Join-Path $output 'EDITION-NOTICE.txt')
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'runtime-cpp\licenses\sqlite.txt') -Destination (Join-Path $licenses 'sqlite.txt')
& cmake --install $build --prefix $output --component Runtime --config Release
if ($LASTEXITCODE -ne 0) { throw 'CMake Runtime 安装或实际依赖许可证复制失败' }
$readmePath=Join-Path $output 'README.md'
$readme=[IO.File]::ReadAllText($readmePath,$encoding)
$editionNotice=if($Edition -eq 'community'){'Community 社区版，Apache-2.0。两种补环境组合、目标 JSCall/Native 完整原值、HTTP 正文和基础搜索/MCP 保留；标注专业版的采集/高级分析不在此内核中。'}else{'Professional 专业版，保留当前全部采集与分析能力；专业独有实现闭源，第三方及共享组件许可见 licenses。'}
$browserNotice=if($NativeChromePath){'随包包含浏览器；打包身份校验不等于原生运行验收，状态以配套验收回执为准。'}else{'本包为 host-only 开发候选，不包含配套定制 Chromium，不是已完成原生验收的整包。'}
[IO.File]::WriteAllText($readmePath,('# 版本与产物状态'+"`n`n"+$editionNotice+"`n`n"+$browserNotice+"`n`n请先阅读《版本拆分说明.md》。下列历史功能说明按当前版本能力与真实验收范围使用。`n`n"+$readme),$encoding)
if ($NativeChromePath) {
    $source = (Resolve-Path -LiteralPath $NativeChromePath).Path
    $config = Join-Path $build ('package-verification-'+[guid]::NewGuid().ToString('N')+'.json')
    [System.IO.File]::WriteAllText($config,(@{ transport='native'; chromePath=$source; url='https://example.test/'; output=(Join-Path $build ('verify-' + [guid]::NewGuid())); scripts=$true } | ConvertTo-Json),$encoding)
    try {
        & (Join-Path $runtime 'v8trace-runtime.exe') plan --config $config --assets $assets
        if ($LASTEXITCODE -ne 0) { throw '定制 Chromium 身份校验失败' }
    } finally {
        $resolved = [IO.Path]::GetFullPath($config)
        if (-not $resolved.StartsWith($build.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^package-verification-[a-f0-9]{32}\.json$') { throw '打包校验临时路径越出构建目录。' }
        Remove-Item -LiteralPath $resolved -Force
    }
    $destination = Join-Path $output 'chromium'
    $browserRoot = Split-Path -Parent $source
    $inventoryFile = Join-Path $browserRoot 'v8trace-runtime-files.json'
    if (Test-Path -LiteralPath $inventoryFile -PathType Leaf) {
        $inventory = Get-Content -LiteralPath $inventoryFile -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($inventory.schemaVersion -ne 1 -or $inventory.protocol -ne 'v8trace/runtime-files/1' -or @($inventory.files).Count -gt 100000 -or @($inventory.files).Count -lt 3 -or $inventory.descriptorSha256 -ne (Get-FileHash -LiteralPath (Join-Path $browserRoot 'v8trace-opcode.json')).Hash.ToLowerInvariant()) { throw '浏览器运行文件清单与身份描述不一致，请重新执行 Chromium Package。' }
        $files = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($relative in $inventory.files) {
            if ($relative -isnot [string] -or [IO.Path]::IsPathRooted($relative) -or -not $files.Add($relative)) { throw '浏览器运行文件清单包含无效或重复路径。' }
            $origin = [IO.Path]::GetFullPath((Join-Path $browserRoot $relative))
            $target = [IO.Path]::GetFullPath((Join-Path $destination $relative))
            if (-not $origin.StartsWith($browserRoot+'\',[StringComparison]::OrdinalIgnoreCase) -or -not $target.StartsWith($destination+'\',[StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $origin -PathType Leaf)) { throw ('浏览器运行文件缺失或越出目录：'+$relative) }
            $item = Get-Item -LiteralPath $origin
            while ($item -and $item.FullName -ne $browserRoot) {
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw ('浏览器运行文件经过重解析路径：'+$relative) }
                $item = if ($item -is [IO.FileInfo]) { $item.Directory } else { $item.Parent }
            }
        }
        foreach ($name in @('chrome.exe','chrome.dll','v8trace-opcode.json')) { if (-not $files.Contains($name)) { throw ('浏览器清单缺少核心文件：'+$name) } }
        foreach ($relative in $files) {
            $target = Join-Path $destination $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $browserRoot $relative) -Destination $target
        }
    } else {
        if ((Test-Path -LiteralPath (Join-Path $browserRoot 'build.ninja')) -or (Test-Path -LiteralPath (Join-Path $browserRoot 'obj'))) { throw '编译目录缺少运行文件清单，请先执行 Chromium Package；不会把 obj / 编译缓存复制到发行包。' }
        Copy-Item -LiteralPath $browserRoot -Destination $destination -Recurse
    }
    & icacls.exe $destination /grant '*S-1-15-2-2:(OI)(CI)(RX)'
    if ($LASTEXITCODE -ne 0) { throw 'Chromium LPAC 权限初始化失败' }
}
$files = @(Get-ChildItem -LiteralPath $output -File -Recurse | ForEach-Object { [ordered]@{ path=$_.FullName.Substring($output.TrimEnd('\').Length + 1); bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } })
$manifest = [ordered]@{ schemaVersion=1; product='V8 Trace Studio'; author='Alcohol'; runtime='cpp'; runtimeVersion='2.0.0'; target='Windows x64'; frontend='WinForms'; nodeRequired=$false; automaticRuntimeFallback=$false; builtAt=[DateTimeOffset]::UtcNow.ToString('o'); codeSigned=$false; files=$files }
$manifest.browserIncluded=[bool]$NativeChromePath
$manifest.edition=$Edition
$manifest.editionProtocol='v8trace/edition/1'
$manifest.sourceLicense=$identity.edition.sourceLicense
$manifest.releaseState=if($NativeChromePath){'browser-bundled-unverified'}else{'host-only-browser-pending'}
$manifest.expectedLayeredCallProtocol='v8trace/call/2'
$manifest.jsvmpDiscovery=if($Edition -eq 'professional'){'offline-indexed-read-candidates'}else{'professional-only'}
$manifest.mcp=[ordered]@{runtime='cpp';tools=16;transports=@('stdio','streamable-http');protocolVersions=@('2026-07-28','2025-11-25');defaultBind='127.0.0.1';defaultPort=17881;globalConfigurationModified=$false;referenceCommit='fd8f5c781c69b10fad463d094b727c79c0d24364'}
$manifest.mcp.tools=if($Edition -eq 'professional'){16}else{11}
[System.IO.File]::WriteAllText((Join-Path $output 'package-manifest.json'),($manifest | ConvertTo-Json -Depth 8),$encoding)
[pscustomobject]@{output=$output;runtime=(Join-Path $runtime 'v8trace-runtime.exe');nodeRequired=$false;files=$files.Count} | ConvertTo-Json
