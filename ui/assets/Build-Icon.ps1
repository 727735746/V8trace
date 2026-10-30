[CmdletBinding()]
param(
    [string]$SourcePng = (Join-Path $PSScriptRoot 'alcohol-v8trace.png'),
    [string]$OutputIco = (Join-Path $PSScriptRoot 'alcohol-v8trace.ico')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (-not ('V8TraceIconHandle' -as [type])) {
    Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class V8TraceIconHandle { [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr icon); }'
}
$source = [System.Drawing.Image]::FromFile([IO.Path]::GetFullPath($SourcePng))
$frames = @()
try {
    if ($source.Width -ne $source.Height -or $source.Width -lt 16) { throw 'The icon source must be square and at least 16 pixels.' }
    foreach ($size in @(16,24,32,48,64,128,256)) {
        $bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            try {
                $graphics.Clear([System.Drawing.Color]::Transparent)
                $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $graphics.DrawImage($source, [System.Drawing.Rectangle]::new(0,0,$size,$size))
            } finally { $graphics.Dispose() }
            $handle = $bitmap.GetHicon()
            try {
                $icon = [System.Drawing.Icon]::FromHandle($handle)
                $buffer = [IO.MemoryStream]::new()
                try { $icon.Save($buffer); $single = $buffer.ToArray() }
                finally { $buffer.Dispose(); $icon.Dispose() }
                $length = [BitConverter]::ToUInt32($single,14)
                $offset = [BitConverter]::ToUInt32($single,18)
                $data = [byte[]]::new($length)
                [Array]::Copy($single,$offset,$data,0,$length)
                $frames += [pscustomobject]@{Size=$size;Data=$data}
            } finally { [void][V8TraceIconHandle]::DestroyIcon($handle) }
        } finally { $bitmap.Dispose() }
    }
} finally { $source.Dispose() }
$destination = [IO.FileStream]::new([IO.Path]::GetFullPath($OutputIco),[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::None)
$writer = [IO.BinaryWriter]::new($destination)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        $dimension = if ($frame.Size -eq 256) { 0 } else { $frame.Size }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension); $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]$frame.Data.Length); $writer.Write([uint32]$offset)
        $offset += $frame.Data.Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame.Data) }
    $writer.Flush(); $destination.Flush($true)
} finally { $writer.Dispose() }
[pscustomobject]@{Icon=[IO.Path]::GetFullPath($OutputIco);Sizes=@($frames.Size);SourceSha256=(Get-FileHash -LiteralPath $SourcePng -Algorithm SHA256).Hash.ToLowerInvariant();IconSha256=(Get-FileHash -LiteralPath $OutputIco -Algorithm SHA256).Hash.ToLowerInvariant()} | ConvertTo-Json -Compress
