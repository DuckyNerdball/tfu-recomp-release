# Play.ps1 - one command to go from "I have the game files" to playing.
#
# What it does:
#   1. Finds your game files: the two disc ISOs, or discs already extracted by an earlier run.
#   2. If the game has not been built yet (or you pass -Rebuild): extracts the discs, merges them,
#      generates the recompiled code and builds this repo as it is now. First build takes about an hour.
#   3. Starts the game.
#
# Usage (from anywhere):
#   .\Play.ps1                                 # ISOs in the folder that contains this repo
#   .\Play.ps1 -Games D:\Isos                  # ISOs somewhere else
#   .\Play.ps1 -Scale 3 -Look clean            # anything after the options below goes to tools\run.ps1
#   .\Play.ps1 -Rebuild                        # rebuild after pulling new code, then play
#   .\Play.ps1 -BuildOnly                      # build, do not start
#
# Needs: git, Visual Studio 2022 C++ tools and clang 20+. The ReXGlue SDK is found automatically (-Sdk <path>,
# TFU_REXGLUE_SDK, the BioShock recomp copy) or, if there is none, downloaded, patched and built by tools\Get-Sdk.ps1.
# No game files are stored in this repo; you must supply your own discs.
param(
    [string]$Games = '',
    [string]$Sdk = '',
    [switch]$Rebuild,
    [switch]$BuildOnly
)
$RunArgs = $args   # everything not named above (e.g. -Scale 3 -Look clean) is passed to tools\run.ps1
$ErrorActionPreference = 'Stop'
$Repo  = $PSScriptRoot
$Root  = Split-Path -Parent $Repo
$Setup = Join-Path $Repo 'tools\Setup-TFURecomp.ps1'
$Run   = Join-Path $Repo 'tools\run.ps1'
$Exe   = Join-Path $Repo 'app\out\build\win-amd64-relwithdebinfo\tfu.exe'
$Game  = Join-Path $Repo 'game_all\default.xex'
function Stop-With($msg) { Write-Host "STOP: $msg" -ForegroundColor Red; exit 1 }

if (-not $Games) { $Games = $Root }
$needBuild = $Rebuild -or -not (Test-Path $Exe) -or -not (Test-Path $Game) -or -not (Test-Path $Run)

if ($needBuild) {
    # --- game files ---
    $isos      = @(Get-ChildItem $Games -Filter '*.iso' -ErrorAction SilentlyContinue)
    $extracted = Test-Path (Join-Path $Repo 'game_disc1\default.xex')
    if ($isos.Count -lt 1 -and -not $extracted) {
        Stop-With "No game files found. Put your two Force Unleashed: Ultimate Sith Edition ISOs in '$Games' (disc 2's file name must contain 'Disc2'), or run: .\Play.ps1 -Games <folder with the ISOs>"
    }
    if ($isos.Count -eq 1 -and -not $extracted) {
        Write-Host "Only one ISO found in $Games. The DLC campaigns (Hoth, Tatooine, Jedi Temple) need disc 2 as well." -ForegroundColor Yellow
    }
    # --- SDK ---
    $cands = @($Sdk, $env:TFU_REXGLUE_SDK,
               'E:\Projects\BioshockProject\BioShockRaptureEdition\bioshock-recomp\.deps\rexglue-sdk',
               (Join-Path $Repo '.deps\rexglue-sdk'),
               (Join-Path $Root 'rexglue-sdk')) | Where-Object { $_ }
    $sdkOk = $cands | Where-Object { Test-Path (Join-Path $_ 'out\install\win-amd64\lib\cmake\rexglue') } | Select-Object -First 1
    if (-not $sdkOk) {
        # none on this PC: download the public SDK, apply sdk\rexglue-tfu.patch and build it (30-60 min, once)
        Write-Host 'No built ReXGlue SDK found. Downloading, patching and building it into .deps\rexglue-sdk (one time, 30-60 min).' -ForegroundColor Yellow
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'tools\Get-Sdk.ps1')
        if ($LASTEXITCODE -ne 0) { Stop-With 'Could not prepare the SDK (see the messages above). Fix that and run Play.ps1 again; it resumes.' }
        $sdkOk = Join-Path $Repo '.deps\rexglue-sdk'
    }
    # --- never build while the game runs (it drops to ~2 fps and the exe is locked) ---
    if (Get-Process tfu -ErrorAction SilentlyContinue) { Stop-With 'The game is running. Close it before building.' }

    Write-Host "Building tfu-recomp (SDK: $sdkOk, game files: $Games)" -ForegroundColor Cyan
    & $Setup -Root $Root -Repo $Repo -IsoDir $Games -Sdk $sdkOk -NoLaunch
    if (-not (Test-Path $Exe) -or -not (Test-Path $Game) -or -not (Test-Path $Run)) {
        Stop-With "Build did not finish. Newest log: $(Join-Path $Repo 'logs') (setup_*.log, build_*.log, codegen_*.log)"
    }
    Write-Host 'Build OK.' -ForegroundColor Green
}

if ($BuildOnly) { exit 0 }
$passArgs = @()
if ($RunArgs) { $passArgs = @($RunArgs | ForEach-Object { "$_" }) }
# a new PowerShell so "-Scale 3 -Look clean" style options are parsed as run.ps1 parameters
& powershell -NoProfile -ExecutionPolicy Bypass -File $Run @passArgs
exit $LASTEXITCODE
