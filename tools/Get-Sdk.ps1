# Get-Sdk.ps1 - download the ReXGlue SDK, apply this project's patches and build it.
# Used by Play.ps1 when no built SDK is found. Re-runnable: finished steps are skipped.
#   .\tools\Get-Sdk.ps1                    # -> <repo>\.deps\rexglue-sdk  (git-ignored)
#   .\tools\Get-Sdk.ps1 -Dest D:\sdk\rex   # somewhere else
# Needs git, Visual Studio 2022 C++ tools and clang 20+. First run: about 30-60 minutes and several GB.
# The SDK is pinned to the exact commit the patch was made against (sdk\BASE_COMMIT.txt).
param([string]$Dest = '')
$ErrorActionPreference = 'Continue'   # native tools write progress to stderr; we check exit codes ourselves
$Repo  = Split-Path -Parent $PSScriptRoot
$Patch = Join-Path $Repo 'sdk\rexglue-tfu.patch'
$Base  = (Get-Content (Join-Path $Repo 'sdk\BASE_COMMIT.txt') -TotalCount 1).Trim()
$Url   = 'https://github.com/rexglue/rexglue-sdk.git'
if (-not $Dest) { $Dest = Join-Path $Repo '.deps\rexglue-sdk' }
$Done  = Join-Path $Dest 'out\install\win-amd64\lib\cmake\rexglue\rexglueConfig.cmake'
$Mark  = Join-Path $Dest '.tfu_patch_applied'
function Stop-With($msg) { Write-Host "STOP (SDK): $msg" -ForegroundColor Red; exit 1 }
function Step($t) { Write-Host "`n--- SDK: $t ---" -ForegroundColor Cyan }

if (Test-Path $Done) { Write-Host "SDK already built: $Dest"; exit 0 }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Stop-With 'git is not installed (winget install Git.Git).' }
if (-not (Test-Path $Patch)) { Stop-With "missing $Patch" }

if (-not (Test-Path (Join-Path $Dest '.git'))) {
    Step "download ($Url)"
    New-Item -ItemType Directory -Force (Split-Path -Parent $Dest) | Out-Null
    git clone $Url $Dest
    if ($LASTEXITCODE -ne 0) { Stop-With 'git clone failed.' }
}
Push-Location $Dest
try {
    if (-not (Test-Path $Mark)) {
        Step "pin to $Base"
        git checkout --force $Base
        if ($LASTEXITCODE -ne 0) { Stop-With "commit $Base not found in the SDK repo." }
        git submodule update --init --recursive
        if ($LASTEXITCODE -ne 0) { Stop-With 'downloading the SDK submodules failed. Run again to resume.' }
        Step 'apply tfu patch'
        git apply --check $Patch
        if ($LASTEXITCODE -ne 0) { Stop-With 'the patch does not apply to this SDK commit.' }
        git apply $Patch
        if ($LASTEXITCODE -ne 0) { Stop-With 'git apply failed.' }
        Set-Content $Mark $Base -Encoding ASCII
    } else { Write-Host 'patch already applied' }

    . (Join-Path $Repo 'tools\vsenv.ps1')   # Visual Studio environment + clang 20+ (throws with a clear message if missing)
    $ErrorActionPreference = 'Continue'
    Step 'configure'
    cmake --preset win-amd64
    if ($LASTEXITCODE -ne 0) { Stop-With 'cmake configure failed.' }
    Step 'build (Release) - this is the long part'
    cmake --build --preset win-amd64-release
    if ($LASTEXITCODE -ne 0) { Stop-With 'SDK build failed.' }
    Step 'install'
    cmake --install (Join-Path $Dest 'out\build\win-amd64') --config Release
    if ($LASTEXITCODE -ne 0) { Stop-With 'SDK install failed.' }
} finally { Pop-Location }
if (-not (Test-Path $Done)) { Stop-With "build finished but $Done is missing." }
Write-Host "SDK ready: $Dest" -ForegroundColor Green
exit 0
