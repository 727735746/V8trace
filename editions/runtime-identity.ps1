function Get-V8TraceRuntimeIdentity {
    param([Parameter(Mandatory=$true)][string]$Runtime)
    $binary=(Resolve-Path -LiteralPath $Runtime -ErrorAction Stop).Path
    $encoding=[Text.UTF8Encoding]::new($false,$true)
    $psi=[Diagnostics.ProcessStartInfo]::new()
    $psi.FileName=$binary;$psi.Arguments='--version';$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true
    $psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $psi.StandardOutputEncoding=$encoding;$psi.StandardErrorEncoding=$encoding
    $process=[Diagnostics.Process]::Start($psi)
    try {
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(-not $process.WaitForExit(15000)) { $process.Kill();$process.WaitForExit();throw 'Runtime identity command timed out.' }
        $wire=$stdout.GetAwaiter().GetResult();$errorText=$stderr.GetAwaiter().GetResult()
        if($process.ExitCode -ne 0){throw ('Runtime identity command failed: '+$process.ExitCode+' '+$errorText)}
        if($wire.Length -gt 65536 -or $errorText.Length){throw 'Unexpected runtime identity response.'}
        return ($wire | ConvertFrom-Json -ErrorAction Stop)
    } finally { $process.Dispose() }
}
