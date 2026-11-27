# Switch NFS Most Wanted between the REAL save and a FRESH test save, by renaming folders in Documents.
#   saves.ps1 status        - which save is active
#   saves.ps1 fresh         - activate the fresh test save (empty = the game asks for a new profile)
#   saves.ps1 real          - activate the real save again
#   saves.ps1 snapshot-fresh - copy the currently active save to H:\NFSMW-saves\fresh-template (after the prologue)
# Close the game first. Nothing is deleted: the inactive save is kept as "NFS Most Wanted.real" / ".fresh".
param([Parameter(Mandatory)][ValidateSet('status','fresh','real','snapshot-fresh')][string]$Mode)
$docs   = [Environment]::GetFolderPath('MyDocuments')
$active = Join-Path $docs 'NFS Most Wanted'
$real   = Join-Path $docs 'NFS Most Wanted.real'
$fresh  = Join-Path $docs 'NFS Most Wanted.fresh'
$marker = '.coop-save-kind'
if (Get-Process speed -ErrorAction SilentlyContinue) { 'Close the game first.'; exit 1 }
function Kind($dir) { if (Test-Path "$dir\$marker") { (Get-Content "$dir\$marker" -Raw).Trim() } elseif (Test-Path $dir) { 'real' } else { 'none' } }
switch ($Mode) {
  'status' { "active: $(Kind $active)   (.real present: $(Test-Path $real), .fresh present: $(Test-Path $fresh))" }
  'fresh' {
    if ((Kind $active) -eq 'fresh') { 'fresh save already active'; break }
    if (Test-Path $real) { "refusing: $real already exists"; exit 1 }
    if (Test-Path $active) { Rename-Item $active $real }
    if (Test-Path $fresh) { Rename-Item $fresh $active }
    elseif (Test-Path 'H:\NFSMW-saves\fresh-template') { Copy-Item 'H:\NFSMW-saves\fresh-template' $active -Recurse }
    else { New-Item -ItemType Directory $active | Out-Null }
    Set-Content "$active\$marker" 'fresh'
    "fresh save active (real save kept at $real)"
  }
  'real' {
    if ((Kind $active) -eq 'real') { 'real save already active'; break }
    if (-not (Test-Path $real)) { "no $real to restore"; exit 1 }
    if (Test-Path $fresh) { "refusing: $fresh already exists"; exit 1 }
    Rename-Item $active $fresh
    Rename-Item $real $active
    "real save active (fresh save kept at $fresh)"
  }
  'snapshot-fresh' {
    $t = 'H:\NFSMW-saves\fresh-template'
    if (Test-Path $t) { Rename-Item $t "$t-old-$(Get-Date -Format yyyyMMdd-HHmmss)" }
    Copy-Item $active $t -Recurse
    Set-Content "$t\$marker" 'fresh'
    "snapshot saved to $t"
  }
}
