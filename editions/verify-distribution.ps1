[CmdletBinding()]
param([string]$PackagePath = $PSScriptRoot,[string]$BindingPath,[switch]$RequireBinding)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'artifact-tools.ps1')
$root = (Resolve-Path -LiteralPath $PackagePath).Path.TrimEnd('\')
$manifestPath = Join-Path $root 'package-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath) -or (Get-Item -LiteralPath $manifestPath).Length -eq 0) { throw '发行清单缺失或为空，请重新解压完整发行包。' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or $manifest.runtime -ne 'cpp' -or @($manifest.files).Count -lt 3) { throw '发行清单格式不符。' }
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($row in $manifest.files) {
    if ([IO.Path]::IsPathRooted($row.path) -or -not $seen.Add($row.path)) { throw '发行清单含绝对路径或重复项。' }
    $path = [IO.Path]::GetFullPath((Join-Path $root $row.path))
    if (-not $path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '发行清单路径越界。' }
    $item = Get-Item -LiteralPath $path -Force
    if ($item.PSIsContainer -or $item.Length -ne $row.bytes -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $row.sha256) { throw ('文件长度或SHA-256不符：' + $row.path) }
}
foreach ($relative in @('V8Trace.exe', 'runtime\v8trace-runtime.exe')) {
    $path = Join-Path $root $relative
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($path))
    try {
        if ($reader.BaseStream.Length -lt 256 -or $reader.ReadUInt16() -ne 0x5A4D) { throw ('EXE为空或缺少MZ头：' + $relative) }
        $reader.BaseStream.Position = 60
        $offset = $reader.ReadUInt32()
        if ($offset -gt $reader.BaseStream.Length - 24) { throw ('EXE的PE位置越界：' + $relative) }
        $reader.BaseStream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw ('EXE不是有效的Windows x64程序：' + $relative) }
    } finally { $reader.Dispose() }
}
$bound=$false
if(-not $BindingPath -and $manifest.PSObject.Properties.Name -contains 'artifactBindingFileName'){
    $name=[string]$manifest.artifactBindingFileName
    if([IO.Path]::GetFileName($name) -ne $name){throw 'External binding name is not a file name.'}
    $candidate=Join-Path (Split-Path -Parent $root) $name
    if(Test-Path -LiteralPath $candidate){$BindingPath=$candidate}
}
if($BindingPath){
    $claim=Get-Content -LiteralPath $BindingPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if(-not [IO.Path]::GetFullPath([string]$claim.package).TrimEnd('\').Equals($root,[StringComparison]::OrdinalIgnoreCase) -or $claim.edition -ne $manifest.edition -or $claim.packageBatchId -ne $manifest.packageBatchId){throw 'Product binding root/edition/batch differs from selected package.'}
    Invoke-V8TraceBuildTool -Tool artifact_binding -Arguments @('check','--binding',$BindingPath);$bound=$true
}
if($RequireBinding -and -not $bound){throw 'Exact external product binding is required for this release check.'}
[pscustomobject]@{ conclusion = $(if($bound){'FILES_AND_BINDING_PASS'}else{'FILES_ONLY_PENDING_BINDING'}); edition = $manifest.edition; checkedFiles = $seen.Count; package = $root; productBindingVerified=$bound; nativeMediaAcceptancePerformed=$false; wholePackageReleaseEligible=$false } | ConvertTo-Json -Compress
