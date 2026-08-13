# clayfray ctl client (Windows twin of ctl.sh): send commands to a running
# clayfray and print the response. One argument per command line:
#
#   tools\ctl.ps1 "set look.aoStrength 0.8" "shot lookdev/probe.png"
#   tools\ctl.ps1 stats
#
# Env: CLAYFRAY_CTL=dir (default .\ctl), CLAYFRAY_CTL_TIMEOUT=sec (default 20).
param([Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Commands)

$dir = if ($env:CLAYFRAY_CTL) { $env:CLAYFRAY_CTL } else { "ctl" }
$timeout = if ($env:CLAYFRAY_CTL_TIMEOUT) { [int]$env:CLAYFRAY_CTL_TIMEOUT } else { 20 }
New-Item -ItemType Directory -Force -Path "$dir/in", "$dir/out" | Out-Null

$name = "{0}_{1}" -f [DateTimeOffset]::Now.ToUnixTimeSeconds(), $PID
$tmp = "$dir/in/.$name.tmp"
# LF endings + no BOM: the poller splits on \n and tolerates \r, but keep it clean
[IO.File]::WriteAllText($tmp, ($Commands -join "`n") + "`n")
Move-Item $tmp "$dir/in/$name"   # rename = atomic hand-off to the poller

$deadline = (Get-Date).AddSeconds($timeout)
while (-not (Test-Path "$dir/out/$name")) {
    if ((Get-Date) -gt $deadline) {
        Write-Error "ctl: timed out after ${timeout}s (is clayfray running?)"
        Remove-Item -ErrorAction SilentlyContinue "$dir/in/$name"
        exit 1
    }
    Start-Sleep -Milliseconds 30
}
Get-Content "$dir/out/$name"
Remove-Item "$dir/out/$name"
