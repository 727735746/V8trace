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
& (Join-Path $PSScriptRoot 'build-cpp.ps1') @PSBoundParameters
