# Builds a SquadMod update for ConflictLauncher, and optionally publishes it.
#
#   .\release.ps1 1.0.1            build + make dist\cgt-squadmod-v1.0.1.zip (nothing is uploaded)
#   .\release.ps1 1.0.1 -Publish   also create GitHub release v1.0.1 with the zip attached
#
# ConflictLauncher reads https://api.github.com/repos/Litzer-dev/cgt-squadmod/releases/latest,
# compares tag_name with <game>\mod_version.txt and extracts the first .zip asset into the game
# folder. So every update must be:
#   - a full release (not draft, not pre-release), tag vX.Y.Z, higher than the last one
#   - exactly one .zip asset with the files at its root
#   - WITHOUT squadmod.ini and dgVoodoo.conf: the launcher only keeps the player's Steam= setting,
#     so shipping them would reset [Display], SteamName and the GRAPHICS choices. The mod uses
#     built-in defaults for any key missing from an old ini, and the launcher's own first install
#     already writes both files.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$Publish,
    [string]$Notes = ""
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$Version = $Version.TrimStart('v', 'V')
if (-not [Version]::TryParse($Version, [ref]$null)) { throw "Version must look like 1.0.1 (got '$Version')" }
$tag = "v$Version"

# 1. build
& cmd /c "`"$root\build.bat`""
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# 2. package
$files = 'dinput8.dll', 'steam_api.dll', 'README.txt'
foreach ($f in $files) { if (-not (Test-Path "$root\release\$f")) { throw "release\$f is missing" } }
$ini = Get-Content "$root\release\squadmod.ini" -Raw
if ($ini -notmatch '(?m)^Debug=0' -or $ini -notmatch '(?m)^LoopbackTest=0' -or $ini -notmatch '(?m)^TunnelTest=0') {
    throw "release\squadmod.ini must have Debug=0, LoopbackTest=0, TunnelTest=0"
}
New-Item -ItemType Directory -Force "$root\dist" | Out-Null
$zip = "$root\dist\cgt-squadmod-$tag.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path ($files | ForEach-Object { "$root\release\$_" }) -DestinationPath $zip
Write-Host "Made $zip"
(Get-FileHash $zip -Algorithm SHA256).Hash

# 3. publish
if ($Publish) {
    if (-not $Notes) { $Notes = "SquadMod $tag. Open ConflictLauncher and press Update, or copy the zip's files into the game folder." }
    git -C $root tag $tag
    git -C $root push origin $tag
    gh release create $tag $zip -R Litzer-dev/cgt-squadmod --title "SquadMod $tag" --notes $Notes --latest
    Write-Host "Published $tag. Check: https://api.github.com/repos/Litzer-dev/cgt-squadmod/releases/latest"
}
