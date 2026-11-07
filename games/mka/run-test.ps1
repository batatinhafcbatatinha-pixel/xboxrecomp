param(
    [ValidateRange(1,3600)][int]$Seconds = 30,
    [string]$BuildDirectory = 'D:\CodexBuilds\mka\build-release',
    [switch]$CaptureFrames,
    [switch]$Legacy,          # NV2A/USB/DSP emulated paths instead of the native HLE profile
    [string]$LiveInputFile = '',
    [string]$ExtraEnv = '',
    [switch]$Show              # visible window (live tests with Windows MCP)
)
# MK Armageddon boot test: the launcher's profile, a watchdog, logs in runs\<timestamp>.
$ErrorActionPreference = 'Stop'
if ($Legacy) { throw 'Legacy hardware execution is prohibited in this native-port branch.' }
$gameExe = Join-Path $BuildDirectory 'mka_recomp.exe'
if (Get-Process -Name mka_recomp -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $gameExe }) { throw 'An MK Armageddon process is already running.' }
$run = ('D:\CodexBuilds\mka\runs\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $run | Out-Null
Get-ChildItem Env:RECOMP_* | ForEach-Object { Remove-Item -LiteralPath ('Env:' + $_.Name) }
$profile = [ordered]@{
    RECOMP_VBLANK='1'
    RECOMP_TITLE_KEVENTS='1'; RECOMP_GUEST_LOCK='1'; RECOMP_GPU='d3d11'; RECOMP_KEYBOARD='1'
    RECOMP_OPTIONS='0'; RECOMP_LANGUAGE='1'
}
if (-not $Legacy) {
    $profile.RECOMP_AUDIO_NATIVE='1'; $profile.RECOMP_DS_NATIVE='1'; $profile.RECOMP_INPUT_NATIVE='1'
    $profile.RECOMP_HLE_PRIMARY='1'; $profile.RECOMP_HLE_NOGPU='1'
}
foreach ($k in $profile.Keys) { Set-Item -LiteralPath ('Env:' + $k) -Value $profile[$k] }
foreach ($pair in ($ExtraEnv -split ';' | Where-Object { $_ -match '=' })) { $kv = $pair -split '=', 2; Set-Item -LiteralPath ('Env:' + $kv[0].Trim()) -Value $kv[1] }
if ($CaptureFrames) { $env:RECOMP_FB_DUMP = Join-Path $run 'frame' }
if ($LiveInputFile) { $env:RECOMP_PAD_LIVE = [System.IO.Path]::GetFullPath($LiveInputFile) }
$env:RECOMP_WATCHDOG_SECS = [string]$Seconds
Get-ChildItem Env:RECOMP_* | ForEach-Object { "$($_.Name)=$($_.Value)" } | Set-Content (Join-Path $run 'env.txt')
(Get-FileHash -LiteralPath $gameExe -Algorithm SHA256).Hash | Set-Content (Join-Path $run 'exe.sha256')
$p = Start-Process -FilePath $gameExe -WorkingDirectory $PSScriptRoot -WindowStyle $(if ($Show) { 'Normal' } else { 'Hidden' }) `
    -RedirectStandardOutput (Join-Path $run 'boot.stdout.log') -RedirectStandardError (Join-Path $run 'boot.stderr.log') -PassThru
$null = $p.Handle
if (-not $p.WaitForExit(($Seconds + 30) * 1000)) { Stop-Process -Id $p.Id -Force; "killed after timeout" | Add-Content (Join-Path $run 'result.txt') }
"exit=$($p.ExitCode)" | Add-Content (Join-Path $run 'result.txt')
Write-Output $run
