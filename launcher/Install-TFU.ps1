# Install-TFU.ps1 - builds The Force Unleashed recomp from the player's own two discs.
# Run by RecompLauncher through Prepare.cmd (-Launcher: no prompts, 'progress <0-1> <text>' lines on stdout),
# or by hand:  .\Prepare.cmd -Disc1:D:\disc1.iso -Disc2:D:\disc2.iso
#
# What it does: checks the build tools, downloads this project's source into <InstallDir>\src, finds or builds
# the patched ReXGlue SDK, extracts the discs, generates and compiles the game, writes <InstallDir>\tfu_ready.txt.
# It never downloads game files. Exit codes: 0 ok, 2 no discs, 3 build tools missing, 4 build failed,
# 6 source download failed.
#
# Developer shortcut: a file <InstallDir>\tfu_src.txt holding the path of an existing checkout makes the step use
# that folder instead of downloading the source (tools\Package-Launcher.ps1 -Dev writes it).
param(
    [string]$InstallDir = $PSScriptRoot,
    [string]$Disc1 = '',
    [string]$Disc2 = '',
    [string]$Branch = 'main',
    [string]$Sdk = '',
    [switch]$Launcher
)
$ErrorActionPreference = 'Continue'   # native tools write to stderr; exit codes are checked by hand
$RepoUrl = 'https://github.com/DuckyNerdball/tfu-recomp-release.git'
$InstallDir = $InstallDir.TrimEnd('\', '/')
$Ready = Join-Path $InstallDir 'tfu_ready.txt'
$Log   = Join-Path $InstallDir 'install.log'
function Progress([double]$f, [string]$t) {
    [Console]::Out.WriteLine('progress ' + $f.ToString('0.00', [Globalization.CultureInfo]::InvariantCulture) + ' ' + $t)
    [Console]::Out.Flush()
}
function Fail([string]$m, [int]$code) {
    [Console]::Out.WriteLine("ERROR: $m")
    Add-Content -LiteralPath $Log -Value "$(Get-Date -Format s) ERROR ($code): $m"
    exit $code
}
function Note([string]$m) { [Console]::Out.WriteLine($m); Add-Content -LiteralPath $Log -Value "$(Get-Date -Format s) $m" }

New-Item -ItemType Directory -Force $InstallDir | Out-Null
Note "Install-TFU: InstallDir=$InstallDir Branch=$Branch Disc1='$Disc1' Disc2='$Disc2'"

# ---- 1. build tools ----
Progress 0.01 'Checking build tools'
$missing = @()
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { $missing += 'Git (winget install Git.Git)' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { & $vswhere -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1 } else { $null }
if (-not $vs) { $missing += 'Visual Studio 2022 Build Tools with the C++ workload' }
function ClangMajor([string]$exe) { $v = & $exe --version 2>$null | Select-Object -First 1; if ($v -match 'version (\d+)\.') { [int]$Matches[1] } else { 0 } }
$clangs = @()
if ($vs) { $clangs += (Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang.exe') }
$clangs += (Join-Path $env:ProgramFiles 'LLVM\bin\clang.exe')
if (-not ($clangs | Where-Object { (Test-Path $_) -and (ClangMajor $_) -ge 20 })) { $missing += 'clang 20 or newer (winget install LLVM.LLVM)' }
if ($missing) { Fail ("These build tools are missing: " + ($missing -join '; ')) 3 }

# ---- 2. source ----
$Src = Join-Path $InstallDir 'src'
$dev = Join-Path $InstallDir 'tfu_src.txt'
if (Test-Path $dev) {
    $Src = (Get-Content -LiteralPath $dev -TotalCount 1).Trim()
    if (-not (Test-Path (Join-Path $Src 'tools\Setup-TFURecomp.ps1'))) { Fail "tfu_src.txt points at '$Src', which is not a tfu-recomp checkout." 6 }
    Note "Using the existing checkout $Src (tfu_src.txt)"
} else {
    Progress 0.03 'Downloading the project source'
    $env:GIT_TERMINAL_PROMPT = '0'   # never wait for a password in a windowless step
    if (Test-Path (Join-Path $Src '.git')) {
        git -C $Src fetch --tags origin
        if ($LASTEXITCODE -ne 0) { Fail 'Could not update the source (git fetch failed). Check your connection and access to the repository.' 6 }
    } else {
        git clone $RepoUrl $Src
        if ($LASTEXITCODE -ne 0) { Fail 'Could not download the source (git clone failed). Check your connection and access to the repository.' 6 }
    }
    git -C $Src checkout --force $Branch
    if ($LASTEXITCODE -ne 0) { Fail "Version '$Branch' was not found in the source." 6 }
    git -C $Src merge --ff-only "origin/$Branch" 2>$null | Out-Null   # a branch moves forward; a tag has nothing to merge
}
$commit = (git -C $Src rev-parse HEAD 2>$null | Select-Object -First 1)
$Exe  = Join-Path $Src 'app\out\build\win-amd64-relwithdebinfo\tfu.exe'
$Game = Join-Path $Src 'game_all\default.xex'
$Run  = Join-Path $Src 'tools\run.ps1'

# ---- 3. already built from this source? ----
$stamp = "tfu-recomp $commit"
if ((Test-Path $Ready) -and (Test-Path $Exe) -and (Test-Path $Game) -and (Test-Path $Run) -and
    ((Get-Content -LiteralPath $Ready -TotalCount 1) -eq $stamp)) {
    Progress 1 'Already built'
    exit 0
}
Remove-Item -LiteralPath $Ready -Force -ErrorAction SilentlyContinue   # a build cut short must never look finished

# ---- 4. game files ----
$extracted = Test-Path (Join-Path $Src 'game_disc1\default.xex')
if (-not $Disc1 -and -not $extracted) { Fail 'No disc image was given (Disc 1).' 2 }
foreach ($d in @($Disc1, $Disc2)) { if ($d -and -not (Test-Path -LiteralPath $d)) { Fail "Disc image not found: $d" 2 } }
if (Get-Process tfu -ErrorAction SilentlyContinue) { Fail 'The game is running. Close it and run setup again.' 4 }

# ---- 5. SDK ----
$cands = @($Sdk, $env:TFU_REXGLUE_SDK,
           'E:\Projects\BioshockProject\BioShockRaptureEdition\bioshock-recomp\.deps\rexglue-sdk',
           (Join-Path $Src '.deps\rexglue-sdk')) | Where-Object { $_ }
$sdkOk = $cands | Where-Object { Test-Path (Join-Path $_ 'out\install\win-amd64\lib\cmake\rexglue') } | Select-Object -First 1
if (-not $sdkOk) {
    Progress 0.05 'Building the graphics SDK (30 to 60 minutes, once)'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Src 'tools\Get-Sdk.ps1')
    if ($LASTEXITCODE -ne 0) { Fail 'The SDK could not be built. See the lines above.' 4 }
    $sdkOk = Join-Path $Src '.deps\rexglue-sdk'
}
Note "SDK: $sdkOk"

# ---- 6. extract, generate, build ----
$env:TFU_LAUNCHER = '1'
$a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Src 'tools\Setup-TFURecomp.ps1'),
       '-Root', $InstallDir, '-Repo', $Src, '-Sdk', $sdkOk, '-NoLaunch')
if ($Disc1) { $a += @('-Iso1', $Disc1) }
if ($Disc2) { $a += @('-Iso2', $Disc2) }
& powershell @a
if (-not (Test-Path $Exe) -or -not (Test-Path $Game) -or -not (Test-Path $Run)) {
    Fail "The build did not finish. Logs: $(Join-Path $Src 'logs') (setup_*.log, codegen_*.log, build_*.log)" 4
}
Set-Content -LiteralPath $Ready -Value $stamp -Encoding ASCII
Progress 1 'Ready to play'
exit 0
