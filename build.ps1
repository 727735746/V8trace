[CmdletBinding()]
param(
    [string]$OutputPath,
    [string]$BuildRoot,
    [string]$NativeChromePath,
    [string]$PackageBatchId,
    [string]$WorkspacePlan,
    [ValidateSet('community','professional')][string]$Edition,
    [ValidateRange(1, 4)][int]$Jobs = 1
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build-cpp.ps1') @PSBoundParameters
