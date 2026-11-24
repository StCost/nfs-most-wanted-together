# Local release (same result as the GitHub "Build" workflow, without Actions):
#   tools\release.ps1 -Version 0.2.0            build, package, publish a pre-release on GitHub
#   tools\release.ps1 -Version 0.2.0 -Final     publish as a normal (latest) release
#   tools\release.ps1 -Version 0.2.0 -DryRun    only build and package (dist\MWTogether-0.2.0.asi)
#   -ChangesFile notes.md                       the "What changed" part of the release notes (markdown)
# Needs: Visual Studio 2022 C++ (x86), and the GitHub CLI (gh) logged in for publishing.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$Final,
    [switch]$DryRun,
    [string]$ChangesFile
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

# 2. package: the release is the one file MWTogether.asi (no zip, no dinput8.dll). The menu logo, the
# default MWTogether.ini (written on the first start, never over an existing one), LICENSE and
# THIRD-PARTY-NOTICES are built into it as resources (src\version.rc).
# Ultimate ASI Loader is a required separate download: see README Install.
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$asi = Join-Path $dist "MWTogether-$Version.asi"
Copy-Item build\MWTogether.asi $asi -Force
Write-Host "Packaged $asi"

# Release notes: a download link first, then what changed (-ChangesFile);
# --generate-notes adds the full changelog link. No install steps, SHA256 or license links here: those
# are in the README, on the release asset and built into the ASI.
$repo = 'https://github.com/StCost/nfs-most-wanted-together'
$notes = Join-Path $dist "MWTogether-$Version-notes.md"
$lines = @("# [Click Me to Download NFS MW Together $Version]($repo/releases/download/v$Version/MWTogether.asi)", '')
$lines += '### What changed', ''
if ($ChangesFile) { $lines += Get-Content $ChangesFile } else { $lines += '- (fill in)' }
Write-Host "MWTogether.asi SHA256 $((Get-FileHash $asi -Algorithm SHA256).Hash.ToLower())"
Set-Content $notes $lines -Encoding utf8
Get-Content $notes | Write-Host
# unversioned name (README / CI); DryRun also writes it, so a local package is ready without tagging
$named = Join-Path $dist 'MWTogether.asi'
Copy-Item $asi $named -Force
if ($DryRun) { return }

# 3. tag + GitHub release: one asset with a fixed name, so README can link releases/latest/download/MWTogether.asi
git tag "v$Version"
git push origin "v$Version"
$flags = @('--notes-file', $notes, '--generate-notes', '--title', "NFS MW Together $Version")
if ($Final) { $flags += '--latest' } else { $flags += '--prerelease' }
gh release create "v$Version" $named @flags
Write-Host "Released v$Version"
