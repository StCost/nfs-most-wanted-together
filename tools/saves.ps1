# Switch NFS Most Wanted between the REAL save and a FRESH test save, by renaming folders in Documents.
#   saves.ps1 status         - which save is active
#   saves.ps1 fresh          - activate the fresh test save (empty = the game asks for a new profile)
#   saves.ps1 real           - activate the real save again
#   saves.ps1 snapshot-fresh - copy the active save to the fresh template (e.g. after the prologue)
# Close the game first. Nothing is deleted: the inactive save is kept as "NFS Most Wanted.real" / ".fresh".
# -Template: where snapshot-fresh keeps the template (default "NFS Most Wanted.fresh-template" in Documents).
# -Docs: the folder holding "NFS Most Wanted" (default: Documents).
param(
  [Parameter(Mandatory)][ValidateSet('status','fresh','real','snapshot-fresh')][string]$Mode,
  [string]$Docs = [Environment]::GetFolderPath('MyDocuments'),
  [string]$Template
)
$active = Join-Path $Docs 'NFS Most Wanted'
$real   = Join-Path $Docs 'NFS Most Wanted.real'
$fresh  = Join-Path $Docs 'NFS Most Wanted.fresh'
if (-not $Template) { $Template = Join-Path $Docs 'NFS Most Wanted.fresh-template' }
$marker = '.coop-save-kind'
$ErrorActionPreference = 'Stop'
if (Get-Process speed, speed2 -ErrorAction SilentlyContinue) { 'Close the game first.'; exit 1 }
function Kind($dir) { if (Test-Path "$dir\$marker") { (Get-Content "$dir\$marker" -Raw).Trim() } elseif (Test-Path $dir) { 'real' } else { 'none' } }
switch ($Mode) {
  'status' { "active: $(Kind $active)   (.real present: $(Test-Path $real), .fresh present: $(Test-Path $fresh))" }
  'fresh' {
    if ((Kind $active) -eq 'fresh') { 'fresh save already active'; break }
    if (Test-Path $real) { "refusing: $real already exists"; exit 1 }
    if (Test-Path $active) { Rename-Item $active $real }
    if (Test-Path $fresh) { Rename-Item $fresh $active }
    elseif (Test-Path $Template) { Copy-Item $Template $active -Recurse }
    else { New-Item -ItemType Directory $active | Out-Null }
    Set-Content "$active\$marker" 'fresh'
    "fresh save active (real save kept at $real)"
  }
  'real' {
    if ((Kind $active) -eq 'real') { 'real save already active'; break }
    if (-not (Test-Path $real)) { "no $real to restore"; exit 1 }
    if (Test-Path $fresh) { "refusing: $fresh already exists"; exit 1 }
    if (Test-Path $active) { Rename-Item $active $fresh }
    Rename-Item $real $active
    "real save active (fresh save kept at $fresh)"
  }
  'snapshot-fresh' {
    if (-not (Test-Path $active)) { "no $active to snapshot"; exit 1 }
    if (Test-Path $Template) { Rename-Item $Template "$Template-old-$(Get-Date -Format yyyyMMdd-HHmmss)" }
    $parent = Split-Path $Template
    if (-not (Test-Path $parent)) { New-Item -ItemType Directory $parent | Out-Null }
    Copy-Item $active $Template -Recurse
    Set-Content "$Template\$marker" 'fresh'
    "snapshot saved to $Template"
  }
}
