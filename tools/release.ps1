# Local release (same result as the GitHub "Build" workflow, without Actions):
#   tools\release.ps1 -Version 0.2.0            build, package, publish a pre-release on GitHub
#   tools\release.ps1 -Version 0.2.0 -Final     publish as a normal (latest) release
#   tools\release.ps1 -Version 0.2.0 -DryRun    only build and package (dist\MWTogether-0.2.0.zip)
# Needs: Visual Studio 2022 C++ (x86), and the GitHub CLI (gh) logged in for publishing.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$Final,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

if (-not $DryRun) {
    if (git status --porcelain) { throw "Uncommitted changes - commit or stash them first." }
    if (git tag --list "v$Version") { throw "Tag v$Version already exists." }
}

# 1. build with the version stamped in; MWT_RELEASE leaves the test commands and debug tools out
$env:MWT_VERSION = $Version
$env:MWT_RELEASE = '1'
& (Join-Path $root "build.bat")
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Remove-Item Env:MWT_VERSION, Env:MWT_RELEASE

# 2. package: scripts\MWTogether.asi / default.ini / logo + README (no dinput8.dll)
# Ultimate ASI Loader is a required separate download: see README Install.
$dist = Join-Path $root 'dist'
$pkg = Join-Path $dist "MWTogether-$Version"
Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $pkg 'scripts') | Out-Null
Copy-Item build\MWTogether.asi (Join-Path $pkg 'scripts')
# settings ship as a default only: unpacking an update over an install never overwrites the player's MWTogether.ini
Copy-Item MWTogether.ini (Join-Path $pkg 'scripts\MWTogether.default.ini')
# main menu Together logo (optional at runtime: the plugin uses it if this file is present)
Copy-Item assets\MWTogether-logo.dds (Join-Path $pkg 'scripts\MWTogether-logo.dds')
Copy-Item README.md, LICENSE, THIRD-PARTY-NOTICES.txt $pkg

$zip = Join-Path $dist "MWTogether-$Version.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$pkg\*" -DestinationPath $zip
Write-Host "Packaged $zip"

# SHA256 of the zip and of every file in it, for the release notes (anyone can check a download against them)
$notes = Join-Path $dist "MWTogether-$Version-notes.md"
$lines = @('### SHA256', '', '| File | SHA256 |', '|---|---|')
$lines += "| MWTogether.zip | ``$((Get-FileHash $zip -Algorithm SHA256).Hash.ToLower())`` |"
Get-ChildItem $pkg -Recurse -File | Sort-Object FullName | % {
    $rel = $_.FullName.Substring($pkg.Length + 1)
    $lines += "| $rel | ``$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())`` |"
}
$lines += '', 'Ultimate ASI Loader is not included. Install the x86/Win32 dinput8.dll next to speed.exe (see README).'
Set-Content $notes $lines -Encoding utf8
Get-Content $notes | Write-Host
# unversioned name (README / CI); DryRun also writes it, so a local package is ready without tagging
Copy-Item $zip (Join-Path $dist 'MWTogether.zip') -Force
$assets = @($zip)
if ($DryRun) { return }

# 3. tag + GitHub release
git tag "v$Version"
git push origin "v$Version"
$flags = @('--notes-file', $notes, '--generate-notes', '--title', "NFS MW Together $Version")
if (-not $Final) { $flags += '--prerelease' }
# the release assets always have the same names, so README can link releases/latest/download/MWTogether.zip
$named = @()
foreach ($a in $assets) {
    $fixed = Join-Path $dist ((Split-Path $a -Leaf) -replace "-$([regex]::Escape($Version))", "")
    Copy-Item $a $fixed -Force
    $named += $fixed
}
gh release create "v$Version" @named @flags
Write-Host "Released v$Version"
