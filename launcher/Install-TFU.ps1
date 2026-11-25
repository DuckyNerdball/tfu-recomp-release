# Install-TFU.ps1 - builds The Force Unleashed recomp from the player's own two discs.
# Run by RecompLauncher through Prepare.cmd (-Launcher: no prompts, 'progress <0-1> <text>' lines on stdout),
# or by hand:  .\Prepare.cmd -Disc1:D:\disc1.iso -Disc2:D:\disc2.iso
#
# What it does: finds or installs the build tools (portable Git; with -AcceptVsLicense also Visual Studio Build
# Tools and LLVM, one administrator prompt), downloads this project's source into <InstallDir>\src, finds or builds
# the patched ReXGlue SDK, extracts the discs, generates and compiles the game, writes <InstallDir>\tfu_ready.txt.
# It never downloads game files. Exit codes: 0 ok, 2 no discs, 3 build tools missing or not installed, 4 build failed,
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
    [switch]$AcceptVsLicense,   # the player accepted Microsoft's licence: setup may install the Build Tools
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

# ---- 1. build tools: found, or installed here ----
# Git: a portable copy in <InstallDir>\tools\git (no administrator rights).
# Visual Studio 2022 Build Tools (C++) and LLVM/clang: the vendors' own installers, run once with administrator
# approval, and only when -AcceptVsLicense was passed (the launcher's "build tools" tick, or by hand).
Progress 0.01 'Checking build tools'
$ProgressPreference = 'SilentlyContinue'
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
$Tmp = if ($env:TEMP) { $env:TEMP } else { $InstallDir }
function Get-File([string]$url, [string]$dst) {
    Note "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $dst -UseBasicParsing -Headers @{ 'User-Agent' = 'tfu-setup' }
    if (-not (Test-Path -LiteralPath $dst) -or (Get-Item -LiteralPath $dst).Length -lt 100kb) { throw "download of $url is incomplete" }
}
function Get-Release([string]$repo, [string]$query) {
    Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases$query" -Headers @{ 'User-Agent' = 'tfu-setup' }
}

# -- Git --
$gitDir = Join-Path $InstallDir 'tools\git'
if (-not (Get-Command git -ErrorAction SilentlyContinue) -and (Test-Path (Join-Path $gitDir 'cmd\git.exe'))) { $env:Path = (Join-Path $gitDir 'cmd') + ';' + $env:Path }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Progress 0.01 'Downloading Git (portable)'
    try {
        $asset = (Get-Release 'git-for-windows/git' '/latest').assets | Where-Object { $_.name -match '^MinGit-[\d.]+-64-bit\.zip$' } | Select-Object -First 1
        if (-not $asset) { throw 'no MinGit 64-bit zip in the latest Git for Windows release' }
        $zip = Join-Path $Tmp $asset.name
        Get-File $asset.browser_download_url $zip
        New-Item -ItemType Directory -Force $gitDir | Out-Null
        Expand-Archive -LiteralPath $zip -DestinationPath $gitDir -Force
        Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
    } catch { Note "Git download failed: $($_.Exception.Message)" }
    if (Test-Path (Join-Path $gitDir 'cmd\git.exe')) { $env:Path = (Join-Path $gitDir 'cmd') + ';' + $env:Path }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail 'Git could not be downloaded. Check your connection, or install Git (https://git-scm.com) and press Set Up again.' 3 }
}

# -- Visual Studio Build Tools and clang --
function ClangMajor([string]$exe) { $v = & $exe --version 2>$null | Select-Object -First 1; if ($v -match 'version (\d+)\.') { [int]$Matches[1] } else { 0 } }
function Find-Vs {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    & $vswhere -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
}
function Find-Clang([string]$vs) {
    $c = @()
    if ($vs) { $c += (Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang.exe') }
    $c += (Join-Path $env:ProgramFiles 'LLVM\bin\clang.exe')
    $c | Where-Object { (Test-Path $_) -and (ClangMajor $_) -ge 20 } | Select-Object -First 1
}
function Quote([string]$x) { return [string][char]39 + $x.Replace([string][char]39, [string][char]39 + [string][char]39) + [string][char]39 }
$vs = Find-Vs
$clang = Find-Clang $vs
if (-not $vs -or -not $clang) {
    $need = @(); if (-not $vs) { $need += 'Visual Studio 2022 Build Tools (C++)' }; if (-not $clang) { $need += 'LLVM/clang 20 or newer' }
    if (-not $AcceptVsLicense) {
        [Console]::Out.WriteLine("This PC is missing: " + ($need -join ' and ') + ".")
        Fail 'Tick "Build tools" on the setup screen so setup can install them (or install them yourself), then press Set Up again.' 3
    }
    $vsExe = ''; $llvmExe = ''
    try {
        if (-not $vs) {
            Progress 0.02 'Downloading the Visual Studio Build Tools installer'
            $vsExe = Join-Path $Tmp 'vs_BuildTools.exe'
            Get-File 'https://aka.ms/vs/17/release/vs_BuildTools.exe' $vsExe
        }
        if (-not $clang) {
            Progress 0.02 'Downloading LLVM/clang'
            # Windows PowerShell hands the release list back as ONE object: unroll it before filtering.
            $all  = Get-Release 'llvm/llvm-project' '?per_page=40'
            $rels = @($all | ForEach-Object { $_ }) | Where-Object { $_.tag_name -match '^llvmorg-(\d+)\.\d+\.\d+$' -and -not $_.prerelease }
            $pick = $null
            foreach ($want in 23, 22, 21, 20) {     # 23 is the version this project is built and tested with
                $pick = $rels | Where-Object { $_.tag_name -match "^llvmorg-$want\." -and ($_.assets | Where-Object { $_.name -match '^LLVM-[\d.]+-win64\.exe$' }) } | Select-Object -First 1
                if ($pick) { break }
            }
            if (-not $pick) { throw 'no LLVM 20-23 Windows installer found in the LLVM releases' }
            $asset = $pick.assets | Where-Object { $_.name -match '^LLVM-[\d.]+-win64\.exe$' } | Select-Object -First 1
            $llvmExe = Join-Path $Tmp $asset.name
            Get-File $asset.browser_download_url $llvmExe
        }
    } catch { Fail "A build tool could not be downloaded ($($_.Exception.Message)). Check your connection and press Set Up again." 3 }

    # One elevated helper runs both installers, so Windows asks for approval once. It runs in a visible window
    # with the RemoteSigned policy (enough for a local file). Do not change either: see docs\STATUS.md 2026-11-25.
    $res  = Join-Path $Tmp 'tfu_tools_result.txt'
    $help = Join-Path $Tmp 'tfu_tools_admin.ps1'
    Remove-Item -LiteralPath $res -Force -ErrorAction SilentlyContinue
    $lines = @('$v = "-"; $l = "-"')
    if ($vsExe)   { $lines += '$p = Start-Process -FilePath ' + (Quote $vsExe) + ' -ArgumentList "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" -Wait -PassThru; $v = $p.ExitCode' }
    if ($llvmExe) { $lines += '$p = Start-Process -FilePath ' + (Quote $llvmExe) + ' -ArgumentList "/S" -Wait -PassThru; $l = $p.ExitCode' }
    $lines += 'Set-Content -LiteralPath ' + (Quote $res) + ' -Value "vs=$v llvm=$l" -Encoding ASCII'
    Set-Content -LiteralPath $help -Value $lines -Encoding ASCII
    Progress 0.03 ('Installing ' + ($need -join ' and ') + ': approve the Windows prompt')
    $proc = $null
    try {
        $proc = Start-Process -FilePath 'powershell.exe' -Verb RunAs -PassThru -ErrorAction Stop `
                -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'RemoteSigned', '-File', ('"' + $help + '"'))
    } catch { $proc = $null }
    if (-not $proc) { Fail 'The build tools were not installed: the Windows administrator prompt was declined or could not be shown. Press Set Up again and approve it, or install the tools yourself.' 3 }
    $t0 = Get-Date
    while ($true) {
        $over = $false
        try { $over = $proc.HasExited } catch { $over = Test-Path -LiteralPath $res }   # no access to the elevated process: go by its result file
        if ($over -or (Test-Path -LiteralPath $res)) { break }
        if (((Get-Date) - $t0).TotalHours -gt 3) { break }
        Start-Sleep -Seconds 30
        Progress 0.03 ("Installing build tools, {0} min so far (10 to 40 is normal)" -f [int]((Get-Date) - $t0).TotalMinutes)
    }
    $codes = if (Test-Path -LiteralPath $res) { (Get-Content -LiteralPath $res -TotalCount 1) } else { 'no result file' }
    Note "Build tool installers finished: $codes  (Visual Studio: 0 or 3010 = ok)"
    Remove-Item -LiteralPath $help, $res -Force -ErrorAction SilentlyContinue
    if ($vsExe)   { Remove-Item -LiteralPath $vsExe -Force -ErrorAction SilentlyContinue }
    if ($llvmExe) { Remove-Item -LiteralPath $llvmExe -Force -ErrorAction SilentlyContinue }
    $vs = Find-Vs
    $clang = Find-Clang $vs
    $still = @(); if (-not $vs) { $still += 'Visual Studio 2022 Build Tools with the C++ workload' }; if (-not $clang) { $still += 'LLVM/clang 20 or newer (https://github.com/llvm/llvm-project/releases)' }
    if ($still) {
        [Console]::Out.WriteLine("Installer results: $codes")
        Fail ("Still missing after the install: " + ($still -join '; ') + ". Install it yourself (a restart may be needed), then press Set Up again.") 3
    }
}
Note "Build tools: git=$((Get-Command git).Source) vs=$vs clang=$clang"

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
    & powershell -NoProfile -ExecutionPolicy RemoteSigned -File (Join-Path $Src 'tools\Get-Sdk.ps1')
    if ($LASTEXITCODE -ne 0) { Fail 'The SDK could not be built. See the lines above.' 4 }
    $sdkOk = Join-Path $Src '.deps\rexglue-sdk'
}
Note "SDK: $sdkOk"

# ---- 6. extract, generate, build ----
$env:TFU_LAUNCHER = '1'
$a = @('-NoProfile', '-ExecutionPolicy', 'RemoteSigned', '-File', (Join-Path $Src 'tools\Setup-TFURecomp.ps1'),
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
