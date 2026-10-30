[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Runtime,[string]$EvidencePath)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSEdition -eq 'Desktop') {
    $env:PSModulePath=(Join-Path $PSHOME 'Modules')+[IO.Path]::PathSeparator+$env:PSModulePath
    Import-Module -Name (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
}
$runtimePath=(Resolve-Path -LiteralPath $Runtime).Path
$root=Join-Path ([IO.Path]::GetTempPath()) ('v8trace-share-paths-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$links=@();$checks=0;$skips=@();$observations=@()
function Remove-OwnedLink([string]$Link) {
    $absolute=[IO.Path]::GetFullPath($Link);$prefix=[IO.Path]::GetFullPath($root).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Owned link cleanup leaves fixture root'}
    $item=Get-Item -LiteralPath $absolute -Force
    if(-not($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Expected owned reparse fixture before cleanup'}
    if($item.PSIsContainer){[IO.Directory]::Delete($absolute,$false)}else{[IO.File]::Delete($absolute)}
}
function Run-Runtime([string[]]$Arguments) {
    $old=$ErrorActionPreference
    try {$ErrorActionPreference='Continue';$lines=& $runtimePath @Arguments 2>&1;$code=$LASTEXITCODE}
    finally {$ErrorActionPreference=$old}
    [pscustomobject]@{exitCode=$code;output=($lines | Out-String)}
}
function Expect-Rejected([string]$Name,[string[]]$Arguments) {
    $r=Run-Runtime $Arguments
    if($r.exitCode -eq 0 -or $r.output -notmatch 'reparse|ordinary|path/handle'){throw ('Expected reparse rejection: '+$Name+' '+$r.output)}
    $script:checks++;$script:observations+=@{name=$Name;exitCode=$r.exitCode;output=$r.output}
}
try {
    $source=Join-Path $root 'source';$outside=Join-Path $root 'outside';New-Item -ItemType Directory -Path $source,$outside | Out-Null
    [IO.File]::WriteAllText((Join-Path $source 'summary.json'),'{"schemaVersion":1,"backend":"chromium-native","integrityConclusion":"partial"}')
    [IO.File]::WriteAllText((Join-Path $outside 'outside.json'),'{"token":"outside-fixture-original"}')
    $before=(Get-FileHash -LiteralPath (Join-Path $outside 'outside.json')).Hash
    $settings=Join-Path $root 'fields.json';[IO.File]::WriteAllText($settings,'{"protocol":"v8trace/share-fields/1","copyOpaque":false,"rules":[]}')
    $entry=Join-Path $source 'junction-entry';New-Item -ItemType Junction -Path $entry -Target $outside | Out-Null;$links+=$entry
    Expect-Rejected 'source-directory-entry' @('share-preview','--source',$source,'--settings',$settings)
    Remove-OwnedLink $entry
    $alias=Join-Path $root 'source-alias';New-Item -ItemType Junction -Path $alias -Target $source | Out-Null;$links+=$alias
    Expect-Rejected 'source-root-junction' @('share-preview','--source',$alias,'--settings',$settings)
    $parent=Join-Path $root 'output-parent';New-Item -ItemType Junction -Path $parent -Target $outside | Out-Null;$links+=$parent
    $preview=Run-Runtime @('share-preview','--source',$source,'--settings',$settings)
    if($preview.exitCode -ne 0){throw $preview.output};$binding=($preview.output | ConvertFrom-Json).previewBinding
    Expect-Rejected 'destination-parent-junction' @('share-copy','--source',$source,'--output',(Join-Path $parent 'new-output'),'--settings',$settings,'--binding',$binding)
    $fileLink=Join-Path $source 'file-link.json'
    try {New-Item -ItemType SymbolicLink -Path $fileLink -Target (Join-Path $outside 'outside.json') | Out-Null;$links+=$fileLink}
    catch {$skips+=('file-symlink-create: '+$_.Exception.Message)}
    if(Test-Path -LiteralPath $fileLink){Expect-Rejected 'source-file-symlink' @('share-preview','--source',$source,'--settings',$settings)}
    if((Get-FileHash -LiteralPath (Join-Path $outside 'outside.json')).Hash -ne $before -or (Test-Path -LiteralPath (Join-Path $outside 'new-output'))){throw 'Outside target changed'}
    $checks++
    $result=@{protocol='v8trace/share-path-test/1';status='passed';checks=$checks;observations=$observations;skips=$skips;outsideTargetUnchanged=$true;runtimeSha256=(Get-FileHash -LiteralPath $runtimePath).Hash.ToLowerInvariant()}
    if($EvidencePath){$evidence=[IO.Path]::GetFullPath($EvidencePath);if(Test-Path -LiteralPath $evidence){throw 'Fresh path evidence required'};New-Item -ItemType Directory -Path $evidence | Out-Null;[IO.File]::WriteAllText((Join-Path $evidence 'result.json'),($result | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))}
    $result | ConvertTo-Json -Depth 8 -Compress
} finally {
    $resolved=[IO.Path]::GetFullPath($root);$parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if((Split-Path -Parent $resolved) -ne $parent -or (Split-Path -Leaf $resolved) -notmatch '^v8trace-share-paths-[a-f0-9]{32}$'){throw 'Owned sharing path fixture cleanup boundary mismatch'}
    foreach($link in $links){if(Test-Path -LiteralPath $link){Remove-OwnedLink $link}}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
