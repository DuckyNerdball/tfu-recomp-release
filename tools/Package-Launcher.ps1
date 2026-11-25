# Package-Launcher.ps1 - build the RecompLauncher package from launcher\.
#   .\tools\Package-Launcher.ps1 -Dev            # out\launcher\ wired to THIS checkout: test with the launcher's
#                                                # Developer tools > Add a build (no download, no full rebuild)
#   .\tools\Package-Launcher.ps1 -Version 0.1.0  # out\TFU-Recomp-v0.1.0-Windows-x64.zip for a GitHub release
# The package holds only scripts and recomplauncher.json. Never add game files or the built game to it.
param([string]$Version = '0.1.0', [switch]$Dev)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
$Src  = Join-Path $Repo 'launcher'
$Out  = Join-Path $Repo 'out\launcher'
$Zip  = Join-Path $Repo "out\TFU-Recomp-v$Version-Windows-x64.zip"
$files = 'recomplauncher.json', 'Prepare.cmd', 'Play.cmd', 'Install-TFU.ps1', 'README.txt'

New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($f in $files) { Copy-Item (Join-Path $Src $f) $Out -Force }
$devFile = Join-Path $Out 'tfu_src.txt'
if ($Dev) {
    Set-Content $devFile $Repo -Encoding ASCII
    Write-Host "Dev package: $Out  (uses the checkout $Repo)" -ForegroundColor Green
    Write-Host 'RecompLauncher > Settings > Developers > Developer tools > Add a build > pick that folder.'
} else {
    Remove-Item $devFile -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $Out 'tfu_ready.txt') -Force -ErrorAction SilentlyContinue
    # A release always builds its own tagged source, not whatever main has moved on to.
    $mf = Join-Path $Out 'recomplauncher.json'
    $j  = [IO.File]::ReadAllText($mf)
    $j2 = $j.Replace('"-Branch", "main"', '"-Branch", "v' + $Version + '"')
    if ($j2 -eq $j) { throw 'recomplauncher.json: "-Branch", "main" not found, cannot pin the release to its tag.' }
    [IO.File]::WriteAllText($mf, $j2, (New-Object Text.UTF8Encoding($false)))
    if (Test-Path $Zip) { Remove-Item $Zip -Force }
    Compress-Archive -Path ($files | ForEach-Object { Join-Path $Out $_ }) -DestinationPath $Zip
    Write-Host "Release package: $Zip" -ForegroundColor Green
    Write-Host "The package builds tag v$Version, so tag and push it before publishing:"
    Write-Host "  gh release create v$Version `"$Zip`" --title `"v$Version`" --notes `"Setup package. Needs your own two discs.`""
}
