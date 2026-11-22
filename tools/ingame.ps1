# Shared helpers for automated in-game runs (dev ASI + [Debug] TestKeys=1).
# Dot-source from other scripts:  . $PSScriptRoot\ingame.ps1
#
# After Skip intros / Quick career start were removed from the user settings, a launch no longer
# walks the intro or loads free roam by itself. Set these hidden Debug keys in a *dev* build
# (ignored in MWT_RELEASE, not shown in menus):
#   [Debug]
#   TestKeys=1
#   TestSkipIntro=1    # skip EA logo, PSA, attract movie
#   TestPressStart=1   # press START on the title screen, including when unfocused
# This script also presses START and uses the `freeroam` test command as a fallback, so a run
# does not stall at "press to continue" if those keys were left off.
#
#   Initialize-MwtInGame -Game 'D:\Games\MWT-release-test'
#   Start-MwtGame          # launch, press START if needed, freeroam, wait until in-world
#   Send-MwtCmd 'pos'
#   Stop-MwtGame

$ErrorActionPreference = 'Continue'
if (-not $script:MwtGame) { $script:MwtGame = 'D:\Games\MWT-release-test' }

function Initialize-MwtInGame {
    param([string]$Game = 'D:\Games\MWT-release-test')
    $script:MwtGame = $Game
    $script:MwtLog  = Join-Path $Game 'scripts\MWTogether.log'
    $script:MwtCmd  = Join-Path $Game 'scripts\MWTogether.cmd'
}

function Read-MwtLog {
    if (-not $script:MwtLog -or -not (Test-Path $script:MwtLog)) { return '' }
    $fs = [IO.File]::Open($script:MwtLog, 'Open', 'Read', 'ReadWrite')
    $sr = New-Object IO.StreamReader($fs)
    $t = $sr.ReadToEnd()
    $sr.Close()
    $t
}

function Send-MwtCmd([string]$c) {
    if (-not $script:MwtCmd) { throw 'Call Initialize-MwtInGame first' }
    $dead = (Get-Date).AddSeconds(8)
    while ((Test-Path $script:MwtCmd) -and (Get-Date) -lt $dead) { Start-Sleep -Milliseconds 200 }
    [IO.File]::WriteAllText($script:MwtCmd, $c + "`r`n")
    $before = Get-Date
    $esc = [regex]::Escape($c)
    while ((Get-Date) -lt $dead) {
        Start-Sleep -Milliseconds 200
        $hit = Select-String -Path $script:MwtLog -Pattern ("Command '" + $esc + "'") -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($hit) { return $true }
    }
    return $false
}

function Wait-MwtLog([string]$pattern, [int]$timeoutSec) {
    $end = (Get-Date).AddSeconds($timeoutSec)
    do {
        if ((Read-MwtLog) -match $pattern) { return $true }
        Start-Sleep -Milliseconds 400
    } while ((Get-Date) -lt $end)
    return $false
}

# If the title splash is up, press START (works unfocused via the test command).
function Send-MwtPressStart {
    $t = Read-MwtLog
    if ($t -match 'WS_MW_LS_Splash|MW_LS_Splash') {
        Send-MwtCmd 'key enter' | Out-Null
        return $true
    }
    return $false
}

# Launch (or attach), get past intro/press-start, load career free roam, wait until in-world.
function Start-MwtGame {
    param([int]$FrontEndTimeoutSec = 90, [int]$WorldTimeoutSec = 60, [switch]$KeepRunning)
    if (-not $script:MwtGame) { Initialize-MwtInGame }
    if (-not $KeepRunning) {
        Stop-Process -Name speed -Force -ErrorAction SilentlyContinue
        Start-Sleep 2
        if (Test-Path $script:MwtLog) { Remove-Item $script:MwtLog -Force -ErrorAction SilentlyContinue }
        if (Test-Path $script:MwtCmd) { Remove-Item $script:MwtCmd -Force -ErrorAction SilentlyContinue }
        Start-Process (Join-Path $script:MwtGame 'speed.exe') -WorkingDirectory $script:MwtGame
    }
    $end = (Get-Date).AddSeconds($FrontEndTimeoutSec)
    $atMenu = $false
    $lastPress = [datetime]'2000-01-01'
    do {
        Start-Sleep -Milliseconds 500
        $t = Read-MwtLog
        if ($t -match 'Game flow state 5 -> 6|Game flow state 4 -> 6') { return $true }
        if ($t -match 'FE screens: \[MainMenu\.fng') { $atMenu = $true; break }
        if ((Get-Date) - $lastPress -gt [TimeSpan]::FromSeconds(2)) {
            if (Send-MwtPressStart) { $lastPress = Get-Date }
        }
    } while ((Get-Date) -lt $end)
    if (-not $atMenu) { throw "The game did not reach the main menu. In a dev build set [Debug] TestSkipIntro=1 and TestPressStart=1 (see docs/TESTING.md)." }
    Send-MwtCmd 'freeroam' | Out-Null
    if (-not (Wait-MwtLog 'Game flow state 5 -> 6|Game flow state 4 -> 6|HUD_SingleRace\.fng' $WorldTimeoutSec)) {
        throw 'freeroam did not reach the world (see the log).'
    }
    return $true
}

function Stop-MwtGame {
    Stop-Process -Name speed -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
}

if ($MyInvocation.InvocationName -ne '.') {
    # running as a script: optional -Start
}
