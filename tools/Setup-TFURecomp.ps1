# Setup-TFURecomp.ps1 - Star Wars: The Force Unleashed (Xbox 360, Ultimate Sith Edition) static recomp bootstrap.
# Re-runnable: every stage skips itself if its output already exists.
# Stages: 1 extract both ISOs  2 disc report (XEX title IDs, hashes, packages)  3 rexglue init
#         4 configure + codegen (auto-adds missing tail-call targets, 6 tries)  5 build (~1 h first time)  6 launch
param(
    [string]$Root = 'E:\Projects\ForceUnleashed',
    [string]$Sdk  = 'E:\Projects\BioshockProject\BioShockRaptureEdition\bioshock-recomp\.deps\rexglue-sdk',
    [switch]$SkipBuild,
    [switch]$NoLaunch,
    [string]$Repo = '',     # this repo (default: <Root>\tfu-recomp). Play.ps1 passes its own folder.
    [string]$IsoDir = '',   # folder holding the two disc ISOs (default: <Root>)
    [string]$Iso1 = '',     # explicit disc 1 / disc 2 image paths (RecompLauncher passes these); override the folder scan
    [string]$Iso2 = ''
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if (-not $Repo)   { $Repo = Join-Path $Root 'tfu-recomp' }
if (-not $IsoDir) { $IsoDir = $Root }
$App    = Join-Path $Repo 'app'
$Logs   = Join-Path $Repo 'logs'
$Tools  = Join-Path $Repo 'tools'
$Disc1  = Join-Path $Repo 'game_disc1'
$Disc2  = Join-Path $Repo 'game_disc2'
$Preset = 'win-amd64-relwithdebinfo'
New-Item -ItemType Directory -Force $Repo, $Logs, $Tools | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
Start-Transcript -Path (Join-Path $Logs "setup_$stamp.log") | Out-Null

# TFU_LAUNCHER=1 (set by launcher\Install-TFU.ps1): also print RecompLauncher 'progress <0-1> <text>' lines
function Progress([double]$f, [string]$t) {
    if (-not $env:TFU_LAUNCHER) { return }
    [Console]::Out.WriteLine('progress ' + $f.ToString('0.00', [Globalization.CultureInfo]::InvariantCulture) + ' ' + $t)
    [Console]::Out.Flush()
}
function Step($t) {
    Write-Host "`n=== $t ===" -ForegroundColor Cyan
    if ($t -match '^(\d)/6\s*(.*)$') { Progress (@(0.10, 0.12, 0.30, 0.34, 0.38, 0.50, 0.97)[[int]$Matches[1]]) $Matches[2] }
}
function Fail($t) { Write-Host "STOP: $t" -ForegroundColor Red; Stop-Transcript | Out-Null; exit 1 }
function BE32([byte[]]$b, [int]$o) { $t = [byte[]]$b[$o..($o + 3)]; [Array]::Reverse($t); [BitConverter]::ToUInt32($t, 0) }

# ---------- git + leak guard ----------
$gi = Join-Path $Repo '.gitignore'
if (-not (Test-Path $gi)) {
@'
# Never commit game-derived material
game_disc*/
game_all/
run/
*.iso
*.xex
*.xbe
*.bin
*.xma
*.bik
dumps/
logs/
out/
app/out/
app/generated/*
!app/generated/rexglue.cmake
'@ | Set-Content $gi -Encoding ASCII
    if (Get-Command git -ErrorAction SilentlyContinue) { Push-Location $Repo; git init -q 2>$null; Pop-Location }
}

if (-not (Select-String -Path $gi -Pattern '^game_all/' -Quiet)) { Add-Content $gi 'game_all/' }

# ---------- 1. extract ----------
Step '1/6 Extract ISOs'
$xiso = (Get-Command extract-xiso -ErrorAction SilentlyContinue).Source
if (-not $xiso) { $xiso = Get-ChildItem $Tools -Recurse -Filter 'extract-xiso.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
if (-not $xiso) {
    $xiso = Get-ChildItem 'E:\Projects' -Recurse -Depth 4 -Filter 'extract-xiso.exe' -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
}
if (-not $xiso) {
    Write-Host 'extract-xiso not found, downloading latest release from XboxDev/extract-xiso'
    $rel   = Invoke-RestMethod 'https://api.github.com/repos/XboxDev/extract-xiso/releases/latest' -Headers @{ 'User-Agent' = 'tfu-setup' }
    $asset = $rel.assets | Where-Object { $_.name -match 'win' -and $_.name -match '\.zip$' } | Select-Object -First 1
    if (-not $asset) { Fail 'No Windows zip in the extract-xiso release. Put extract-xiso.exe in PATH and rerun.' }
    $zip = Join-Path $Tools $asset.name
    Invoke-WebRequest $asset.browser_download_url -OutFile $zip
    Expand-Archive $zip (Join-Path $Tools 'extract-xiso') -Force
    $xiso = Get-ChildItem (Join-Path $Tools 'extract-xiso') -Recurse -Filter 'extract-xiso.exe' | Select-Object -First 1 -ExpandProperty FullName
}
Write-Host "extract-xiso: $xiso"

$explicit = @{}   # full path -> destination folder, when the discs are named one by one
if ($Iso1 -or $Iso2) {
    $isos = @()
    foreach ($e in @(@($Iso1, $Disc1), @($Iso2, $Disc2))) {
        if (-not $e[0]) { continue }
        if (-not (Test-Path -LiteralPath $e[0])) { Fail "Disc image not found: $($e[0])" }
        $it = Get-Item -LiteralPath $e[0]; $isos += $it; $explicit[$it.FullName] = $e[1]
    }
} else {
    $isos = @(Get-ChildItem $IsoDir -Filter '*.iso' -ErrorAction SilentlyContinue | Sort-Object Name)
}
$pairs = @()
if ($isos.Count -lt 1) {
    # no ISOs: fine if the discs were extracted earlier
    if (-not (Test-Path (Join-Path $Disc1 'default.xex'))) { Fail "No .iso files in $IsoDir and no extracted game in $Disc1" }
    Write-Host "no ISOs in $IsoDir; using the discs already extracted in the repo"
    foreach ($d in @($Disc1, $Disc2)) {
        if (Test-Path $d) { $pairs += [pscustomobject]@{ Iso = [pscustomobject]@{ Name = '(already extracted)' }; Dest = $d } }
    }
}
foreach ($iso in $isos) {
    $dest = if ($explicit.ContainsKey($iso.FullName)) { $explicit[$iso.FullName] } elseif ($iso.Name -match 'Disc2') { $Disc2 } else { $Disc1 }
    $pairs += [pscustomobject]@{ Iso = $iso; Dest = $dest }
    if ((Test-Path $dest) -and (Get-ChildItem $dest -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1)) {
        Write-Host "skip: $($iso.Name) already extracted to $dest"; continue
    }
    # extract-xiso wants options BEFORE the file, and creates -d itself (fails if it exists)
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    Write-Host "extracting $($iso.Name) -> $dest"
    Push-Location (Split-Path $dest)
    & $xiso -d $dest -x $iso.FullName
    Pop-Location
    if ($LASTEXITCODE -ne 0) { Fail "extract-xiso failed on $($iso.Name)" }
}

# ---------- 2. disc report ----------
Step '2/6 Disc report'
$report = Join-Path $Logs 'disc_report.txt'
$lines  = @("TFU disc report $stamp", '')
$xexes  = @()
foreach ($p in $pairs) {
    $files = Get-ChildItem $p.Dest -Recurse -File
    $sum   = ($files | Measure-Object Length -Sum).Sum
    $lines += "== $($p.Iso.Name) -> $(Split-Path $p.Dest -Leaf): $($files.Count) files, $([math]::Round($sum/1GB,2)) GB"
    $lines += ($files | Where-Object { $_.DirectoryName -eq $p.Dest } | ForEach-Object { "   root: $($_.Name)  $($_.Length)" })
    $dirs  = Get-ChildItem $p.Dest -Directory | ForEach-Object { $_.Name }
    $lines += "   dirs: $($dirs -join ', ')"
    foreach ($f in $files) {
        $fs = [IO.File]::OpenRead($f.FullName)
        try { $hdr = New-Object byte[] 4; [void]$fs.Read($hdr, 0, 4) } finally { $fs.Close() }
        $magic = [Text.Encoding]::ASCII.GetString($hdr)
        if ($magic -in 'LIVE', 'PIRS', 'CON ') { $lines += "   package [$magic]: $($f.FullName.Substring($p.Dest.Length+1))  $($f.Length)" }
        if ($magic -eq 'XEX2') {
            $fs = [IO.File]::OpenRead($f.FullName)
            try { $b = New-Object byte[] 0x10000; [void]$fs.Read($b, 0, $b.Length) } finally { $fs.Close() }
            $n = BE32 $b 0x14; $title = '?'; $media = '?'; $ver = '?'; $disc = '?'
            for ($i = 0; $i -lt $n; $i++) {
                $k = BE32 $b (0x18 + 8 * $i); $v = BE32 $b (0x1C + 8 * $i)
                if ($k -eq 0x00040006 -and $v -lt 0xFFF0) {
                    $media = '{0:X8}' -f (BE32 $b $v)
                    $vv    = BE32 $b ($v + 4); $ver = '{0}.{1}.{2}.{3}' -f ($vv -shr 28), (($vv -shr 24) -band 0xF), (($vv -shr 8) -band 0xFFFF), ($vv -band 0xFF)
                    $title = '{0:X8}' -f (BE32 $b ($v + 12))
                    $disc  = "$($b[$v+18])/$($b[$v+19])"
                }
            }
            $sha = (Get-FileHash $f.FullName -Algorithm SHA1).Hash
            $rel = $f.FullName.Substring($p.Dest.Length + 1)
            $lines += "   XEX: $rel  size $($f.Length)  title $title  media $media  v$ver  disc $disc  sha1 $sha"
            $xexes += [pscustomobject]@{ Path = $f.FullName; Rel = $rel; Size = $f.Length; Title = $title; Sha = $sha; Disc = $p.Dest }
        }
    }
    $lines += ''
}
$lines | Set-Content $report -Encoding UTF8
$lines | ForEach-Object { Write-Host $_ }

# Target = default.xex on disc 1 (the game). Disc 2 is expected to be DLC/installer; the report shows what it really is.
$target = $xexes | Where-Object { $_.Disc -eq $Disc1 -and $_.Rel -ieq 'default.xex' } | Select-Object -First 1
if (-not $target) { $target = $xexes | Where-Object { $_.Disc -eq $Disc1 } | Sort-Object Size -Descending | Select-Object -First 1 }
if (-not $target) { Fail 'No XEX found on disc 1. Send me logs\disc_report.txt.' }
Write-Host "Recomp target: $($target.Rel)  title $($target.Title)  sha1 $($target.Sha)" -ForegroundColor Green
"TARGET $($target.Path) sha1 $($target.Sha)" | Add-Content $report

# ---------- 2b. merged game root ----------
# The game is split across both discs (same title ID, different media IDs). The recomp gets one root: disc 1, plus
# every disc 2 file that disc 1 lacks, as hardlinks (same drive, no extra space). Conflicts are logged, disc 1 wins.
$All = Join-Path $Repo 'game_all'
if (-not (Test-Path (Join-Path $All 'default.xex'))) {
    Step '2b Merge discs into game_all (hardlinks)'
    $conf = @(); $n = 0
    foreach ($src in @($Disc1, $Disc2)) {
        if (-not (Test-Path $src)) { continue }
        foreach ($f in Get-ChildItem $src -Recurse -File) {
            $rel = $f.FullName.Substring($src.Length + 1)
            $dst = Join-Path $All $rel
            if (Test-Path -LiteralPath $dst) {
                $have = (Get-Item -LiteralPath $dst).Length
                if ($have -ne $f.Length) { $conf += "$rel  disc1=$have  disc2=$($f.Length)" }
                continue
            }
            $dir = Split-Path $dst
            if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
            New-Item -ItemType HardLink -Path $dst -Target $f.FullName | Out-Null
            $n++
        }
    }
    Write-Host "game_all: $n files linked, $($conf.Count) same-path size conflicts (disc 1 kept)"
    "`nMERGE: $n files in game_all; conflicts (disc 1 kept):" | Add-Content $report
    $conf | Add-Content $report
    # ExternalPacks\List.txt is the pack registry and differs per disc: disc 2's adds the DLC campaigns (Hoth, Tatooine,
    # Jedi Temple), their costumes and credits. Write the union (disc 1 order, then disc-2-only lines) as a real file.
    $l1 = Join-Path $Disc1 'ExternalPacks\List.txt'; $l2 = Join-Path $Disc2 'ExternalPacks\List.txt'
    if ((Test-Path $l1) -and (Test-Path $l2)) {
        $a = [IO.File]::ReadAllText($l1, [Text.Encoding]::GetEncoding(28591)) -split "`r`n"
        $b = [IO.File]::ReadAllText($l2, [Text.Encoding]::GetEncoding(28591)) -split "`r`n"
        $seen = @{}; foreach ($x in $a) { if ($x.Trim()) { $seen[$x.Trim()] = 1 } }
        $extra = @($b | Where-Object { $_.Trim() -and -not $seen.ContainsKey($_.Trim()) })
        $a = @($a | Where-Object { $_ -ne '' })
        $dst = Join-Path $All 'ExternalPacks\List.txt'
        Remove-Item -LiteralPath $dst -Force
        [IO.File]::WriteAllText($dst, (($a + $extra) -join "`r`n") + "`r`n", [Text.Encoding]::GetEncoding(28591))
        Write-Host "List.txt: merged $($extra.Count) disc-2-only pack entries (DLC campaigns, costumes)"
    }
} else { Write-Host 'skip: game_all already built' }
# Campaign variants of the pack registry (2026-11-17): registering the DLC campaigns (disc 2's List.txt lines) makes
# the front end offer ONLY the DLC campaigns - the story vanished after the union went in. run.ps1 -Campaign picks one
# and writes it to List.txt before launch. story = disc 1's list verbatim, dlc = union (disc 1 + disc-2-only lines).
$ext = Join-Path $All 'ExternalPacks'
$story = Join-Path $ext 'List_story.txt'; $dlc = Join-Path $ext 'List_dlc.txt'
if (-not (Test-Path $story)) { [IO.File]::WriteAllBytes($story, [IO.File]::ReadAllBytes((Join-Path $Disc1 'ExternalPacks\List.txt'))) }
if (-not (Test-Path $dlc)) {
    $cur = Join-Path $ext 'List.txt'
    $isUnion = (Get-Item $cur).Length -gt (Get-Item (Join-Path $Disc1 'ExternalPacks\List.txt')).Length
    if ($isUnion) { [IO.File]::WriteAllBytes($dlc, [IO.File]::ReadAllBytes($cur)) } else { Write-Host 'WARN: List.txt is not the union; List_dlc.txt not written' -ForegroundColor Yellow }
}
# all (experiment 2026-11-17): the picker lists REGISTERED campaigns only (DLC packs each hold one .scumcampaign.xml);
# the story campaign Scum/runtime/prefs/TheForceUnleashed.scumcampaign.xml lives in LevelPacks\gp_x360.lp. Register it
# like a DLC campaign: List line + ExternalPacks\theforceunleashed.scumcampaign.xml_x360.lp = hard link to gp_x360.lp.
$listAll = Join-Path $ext 'List_all.txt'   # NB: not $all - PowerShell names are case-insensitive ($All = game_all)
$spk = Join-Path $ext 'theforceunleashed.scumcampaign.xml_x360.lp'
# v1 linked gp_x360.lp itself: picker showed all 4, but gp loaded twice -> CRASH ~3-5 s into the story level
# (sub_8272B180 read 0x8, 21:21:33 and 21:24:21 with a fresh profile). v2: the campaign asset is already resident
# (gp is a global pack), so give the registry entry a tiny UNUSED pack: disc 2's lightside.dds (unregistered texture).
$spkSrc = Join-Path $All 'LevelPacks\lightside.dds_x360.lp'
if ((Test-Path $spk) -and (Get-Item $spk).Length -ne (Get-Item $spkSrc).Length) { Remove-Item -LiteralPath $spk -Force }
if (-not (Test-Path $spk)) { New-Item -ItemType HardLink -Path $spk -Target $spkSrc | Out-Null }
if ((Test-Path $dlc) -and -not (Test-Path $listAll)) {
    $t = [IO.File]::ReadAllText($dlc, [Text.Encoding]::GetEncoding(28591)).TrimEnd("`r", "`n")
    [IO.File]::WriteAllText($listAll, $t + "`r`nTheForceUnleashed.scumcampaign.xml campaign 0xFFFFFFFF`r`n", [Text.Encoding]::GetEncoding(28591))
}
# storyx (experiment 2026-11-17): story registry + the DLC offer string table, no DLC campaigns
$storyx = Join-Path $ext 'List_storyx.txt'
if (-not (Test-Path $storyx)) {
    $t = [IO.File]::ReadAllText($story, [Text.Encoding]::GetEncoding(28591)).TrimEnd("`r", "`n")
    [IO.File]::WriteAllText($storyx, $t + "`r`nDarkSideStr.utf16 offerdata 0xFFFFFFFF`r`n", [Text.Encoding]::GetEncoding(28591))
}

# ---------- VS dev shell ----------
$vsenv = @('E:\Projects\BioshockInfinite\bioshockinf-recomp\tools\vsenv.ps1',
           (Join-Path (Split-Path (Split-Path $Sdk)) 'tools\vsenv.ps1'),
           (Join-Path $Tools 'vsenv.ps1')) | Where-Object { Test-Path $_ } | Select-Object -First 1   # last = this repo's own copy
if (-not $vsenv) { Fail 'vsenv.ps1 not found (tools\vsenv.ps1 is missing from the repo).' }
if ($vsenv -ne (Join-Path $Tools 'vsenv.ps1')) { Copy-Item $vsenv (Join-Path $Tools 'vsenv.ps1') -Force }
. (Join-Path $Tools 'vsenv.ps1')

# ---------- 3. rexglue init ----------
Step '3/6 rexglue init'
$rexDir = Join-Path $Sdk 'out\install\win-amd64\lib\cmake\rexglue'
if (-not (Test-Path $rexDir)) { Fail "ReXGlue SDK install not found at $rexDir" }
$rex = Get-ChildItem (Join-Path $Sdk 'out') -Recurse -Depth 6 -Filter 'rexglue.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if (-not $rex) { Fail "rexglue.exe not found under $Sdk\out" }
$manifest = Join-Path $App 'tfu_manifest.toml'
if (-not (Test-Path (Join-Path $App 'CMakeLists.txt'))) {
    & $rex init --project-name tfu --xex-path $target.Path --game-root $Disc1 --project-root $App
    if ($LASTEXITCODE -ne 0) { Fail 'rexglue init failed (see transcript)' }
} else { Write-Host 'skip: app already initialised' }
$cm = Join-Path $App 'CMakeLists.txt'
$cmText = Get-Content $cm -Raw
if ($cmText -notmatch 'GPU_PLUGINS') {
    $cmText = $cmText -replace 'rexglue_setup_target\(\s*tfu\s*\)', 'rexglue_setup_target(tfu GPU_PLUGINS xenos)'
    Set-Content $cm $cmText -Encoding ASCII
    Write-Host 'CMakeLists: added GPU_PLUGINS xenos'
}
if (-not (Test-Path $manifest)) { $manifest = Get-ChildItem $App -Filter '*manifest*.toml' | Select-Object -First 1 -ExpandProperty FullName }

# ---------- 4. configure + codegen ----------
Step '4/6 Configure + codegen'
$ErrorActionPreference = 'Continue'   # PS 5.1 turns native stderr into terminating errors under 'Stop'; we check $LASTEXITCODE instead
Push-Location $App
function Configure {
    cmake --preset $Preset "-Drexglue_DIR=$($rexDir -replace '\\','/')" 2>&1 | Tee-Object (Join-Path $Logs "configure_$stamp.log") | Out-Null
    if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "configure failed: logs\configure_$stamp.log" }
}
# TOML rejects duplicate keys (ReXGlue then misreports it as "no [project] section"): drop repeated function entries
$seen = @{}; $dedup = foreach ($l in (Get-Content $manifest)) {
    if ($l -match '^"(0x[0-9A-Fa-f]{8})"\s*=') { $k = $Matches[1].ToUpper(); if ($seen.ContainsKey($k)) { continue }; $seen[$k] = 1 }
    $l }
Set-Content $manifest $dedup -Encoding ASCII
Configure
$ok = $false
for ($try = 1; $try -le 6; $try++) {
    $cgLog = Join-Path $Logs "codegen_${stamp}_$try.log"
    cmake --build --preset $Preset --target tfu_codegen 2>&1 | Tee-Object $cgLog | Out-Null
    if ($LASTEXITCODE -eq 0) { $ok = $true; break }
    # ReXGlue line format: "0xTARGET from 0xSOURCE: b 0xTARGET ... - target not in any function" -> keep the FIRST address
    $targets = Select-String -Path $cgLog -Pattern '^\s*(0x8[0-9A-Fa-f]{7}) from 0x8[0-9A-Fa-f]{7}' | ForEach-Object {
        '0x' + $_.Matches[0].Groups[1].Value.Substring(2).ToUpper() } | Sort-Object -Unique
    $text = Get-Content $manifest -Raw
    $new  = $targets | Where-Object { $text -notmatch [regex]::Escape("`"$_`"") }
    if (-not $new) { Pop-Location; Fail "codegen failed and no new addresses to add. Send me $cgLog" }
    if ($text -notmatch '\[entrypoint\.functions\]') { $text = $text.TrimEnd() + "`r`n`r`n[entrypoint.functions]`r`n" }
    $ins  = ($new | ForEach-Object { "`"$_`" = {}" }) -join "`r`n"
    $text = $text -replace '(\[entrypoint\.functions\][^\r\n]*\r?\n)', "`$1$ins`r`n"
    Set-Content $manifest $text -Encoding ASCII
    Write-Host "codegen try $try failed; added $($new -join ', ') to manifest, retrying" -ForegroundColor Yellow
}
if (-not $ok) { Pop-Location; Fail 'codegen still failing after 6 tries. Send me the newest logs\codegen_*.log' }
Configure   # v0.10.0 only picks up generated sources.cmake at configure time

# ---------- 5. build ----------
$exe = Join-Path $App "out\build\$Preset\tfu.exe"
if (-not $SkipBuild) {
    Step '5/6 Build (first build ~1 h)'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    cmake --build --preset $Preset 2>&1 | Tee-Object (Join-Path $Logs "build_$stamp.log") | Select-String -Pattern '^\[\d+/\d+\]|error' | ForEach-Object {
        Write-Host $_.Line
        if ($_.Line -match '^\[(\d+)/(\d+)\]' -and ([int]$Matches[1] % 8) -eq 0) { Progress (0.50 + 0.46 * [int]$Matches[1] / [math]::Max(1, [int]$Matches[2])) "Building ($($Matches[1]) of $($Matches[2]))" }
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) { Pop-Location; Fail "build failed: logs\build_$stamp.log" }
    Write-Host "built in $([int]$sw.Elapsed.TotalMinutes) min"
    $bin = Join-Path $Sdk 'out\install\win-amd64\bin'
    Copy-Item (Join-Path $bin 'rexruntime.dll'), (Join-Path $bin 'rexgpu-xenos.dll') (Split-Path $exe) -Force -ErrorAction SilentlyContinue
}
Pop-Location

# ---------- 6. launcher + first run ----------
Step '6/6 Launch'
$run = Join-Path $Tools 'run.ps1'
@"
# TFU recomp launcher. Newest logs\run_*.log is what to send Claude.
#   -Scale 1..4   internal render scale (1 = native 1280x720, 2 = 2560x1440, 3 = 3840x2160)
#   -Effect       output filter: cas (sharpen, default), fsr (upscale+sharpen), bilinear (stock, soft)
#   -Sharp 0..1   extra CAS sharpness
#   -Diag         kernel/file-system logging at trace (save/load debugging; bigger log)
#   -Fps 60       EXPERIMENT: guest vblank at 2x Fps Hz (game waits 2 vblanks per frame). Watch for 2x game speed.
#   -Probe        log post-processing draws + their constants + real game fps (for Claude)
#   -Look clean   Ghost 0 + Bloom 0.5 + Dof 0 (override any of them below)
#   -Ghost 0..1   frame-blend pass 2D1318F3 (stock 0.5 = 50% of the previous frame -> blurry when moving)
#   -Bloom x      bloom pass 0A2F2B62 constant c6 multiplier (1 = stock)
#   -Dof x        depth-of-field pass 64B214B3 constants c8/c15 multiplier (1 = stock, 0 = off)
#   -Campaign     all (default: story + Jedi Temple + Hoth + Tatooine in New Game, profile run\userdata_allfresh)
#                 dlc (DLC only, profile run\userdata) | story (story only, profile run\userdata_story)
#   -Profile x    use run\userdata_x instead (experiments on a copy of a save)
#   -Campaign storyx  EXPERIMENT: story registry + DarkSideStr offerdata (can DLC costumes load in a story save?)
#   -BackupOnly  back up the save to saves\backup_<time> (done on every launch anyway) and exit
param([int]`$Scale = 2, [ValidateSet('cas','fsr','bilinear')][string]`$Effect = 'cas', [double]`$Sharp = 0.5, [switch]`$Diag, [int]`$Fps = 30, [switch]`$Probe,
      [ValidateSet('stock','clean')][string]`$Look = 'stock', [double]`$Ghost = -1, [double]`$Bloom = -1, [double]`$Dof = -1, [string[]]`$Extra = @(), [switch]`$BackupOnly,
      [ValidateSet('story','storyx','dlc','all')][string]`$Campaign = 'all', [string]`$Profile = '')
`$t   = Get-Date -Format 'yyyyMMdd_HHmmss'
`$exe = '$exe'
`$log = '$Logs\run_' + `$t + '.log'
`$a = @('--gpu_plugin','xenos','--game_data_root','$All','--log_file',`$log,'--log_flush_interval','1',
       "--resolution_scale=`$Scale", "--present_effect=`$Effect", "--present_cas_additional_sharpness=`$Sharp",
       '--anisotropic_override=5') + `$Extra
# Profile + saves per campaign set: a profile that has played DLC crashes the story registry on Press Start
# (2026-11-17 19:59), so -Campaign story gets its own userdata (= the 2026-11-16 setup that played the Vader prologue).
`$udr = if (`$Profile) { '$Repo\run\userdata_' + `$Profile } elseif (`$Campaign -like 'story*') { '$Repo\run\userdata_story' } elseif (`$Campaign -eq 'all') { '$Repo\run\userdata_allfresh' } else { '$Repo\run\userdata' }
`$env:TFU_CAMPAIGN = `$Campaign   # dev menu: flags DLC costumes that break a story save
if (-not (Test-Path `$udr) -and (Test-Path '$Repo\run\userdata\cache')) {   # share the shader cache, nothing else
    New-Item -ItemType Directory -Force `$udr | Out-Null
    Copy-Item '$Repo\run\userdata\cache' `$udr -Recurse -Force
}
New-Item -ItemType Directory -Force `$udr | Out-Null
`$a += @('--user_data_root', `$udr)
# save backup: every SWTFU.BIN* file under run\ (all profiles) -> saves\backup_<stamp>\<same relative path>; keep newest 10
`$ud = '$Repo\run'
`$sv = Get-ChildItem `$ud -Recurse -File -Filter 'SWTFU.BIN*' -ErrorAction SilentlyContinue
if (`$sv) {
    `$bk = '$Repo\saves\backup_' + `$t
    foreach (`$f in `$sv) {
        `$dst = Join-Path `$bk `$f.FullName.Substring(`$ud.Length + 1)
        New-Item -ItemType Directory -Force (Split-Path `$dst) | Out-Null
        Copy-Item -LiteralPath `$f.FullName `$dst -Force
    }
    `$old = @(Get-ChildItem '$Repo\saves' -Directory -Filter 'backup_*' | Sort-Object Name -Descending | Select-Object -Skip 10)
    `$old | Remove-Item -Recurse -Force
    `$m = "save backup: `$(`$sv.Count) file(s), `$((`$sv | Measure-Object Length -Sum).Sum) bytes -> `$bk (pruned `$(`$old.Count))"
    `$m; `$m | Out-File -Encoding ascii (`$log + '.backup.txt')
} else { 'save backup: no SWTFU.BIN under userdata, skipped' }
if (`$BackupOnly) { return }
# pack registry for the chosen campaign set (Remove first: never write through a hard link into game_disc*)
`$lsrc = '$All\ExternalPacks\List_' + `$Campaign + '.txt'
if (Test-Path `$lsrc) {
    Remove-Item -LiteralPath '$All\ExternalPacks\List.txt' -Force -ErrorAction SilentlyContinue
    [IO.File]::WriteAllBytes('$All\ExternalPacks\List.txt', [IO.File]::ReadAllBytes(`$lsrc))
    `$m = "campaign: `$Campaign (`$lsrc), profile `$udr"; `$m; `$m | Out-File -Encoding ascii (`$log + '.campaign.txt')
} else { Write-Host "campaign: `$lsrc missing, List.txt left as is" -ForegroundColor Yellow }
if (`$Look -eq 'clean') { if (`$Ghost -lt 0) { `$Ghost = 0 }; if (`$Bloom -lt 0) { `$Bloom = 0.5 }; if (`$Dof -lt 0) { `$Dof = 0 } }
`$mul = @()
if (`$Ghost -ge 0) { `$f = `$Ghost / 0.5; `$mul += 0..2 | ForEach-Object { "2D1318F346A26035:4:`${_}:`$f" } }
if (`$Bloom -ge 0) { `$mul += 0..3 | ForEach-Object { "0A2F2B62E884F6D2:6:`${_}:`$Bloom" } }
if (`$Dof -ge 0) { foreach (`$c in 8, 15) { `$mul += 0..3 | ForEach-Object { "64B214B3CA33BC3C:`${c}:`${_}:`$Dof" } } }
if (`$mul) { `$a += @("--ps_const_mul=`$(`$mul -join ',')") }
if (`$Fps -ne 30) { `$a += @("--video_mode_refresh_rate=`$([math]::Min(240, `$Fps * 2))") }
if (`$Probe) { `$a += @('--log_quad_draws=true', '--hitch_log_ms=100000', '--ps_const_log=0A2F2B62E884F6D2,64B214B3CA33BC3C,1E0556FC0BD010D1,B9B3A5C4290A5DBB,27E260BDB647DC07,86EFC2BBF933C1EB,2D1318F346A26035,BF2AFFB06B6FB7F9') }
if (`$Diag) { `$env:TFU_DIAG = '1'; `$a += @('--log_max_file_size_mb=200') } else { Remove-Item Env:TFU_DIAG -ErrorAction SilentlyContinue }
"args: `$(`$a -join ' ')" | Set-Content (`$log + '.args.txt')
Set-Location '$Repo'   # crash_trace.cpp writes logs\crash_*.txt relative to the working dir
& `$exe @a
"@ | Set-Content $run -Encoding ASCII
if ((Test-Path $exe) -and -not $NoLaunch) {
    Start-Process powershell -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$run`""
    Write-Host 'Launched. When it crashes or stalls, send me the newest logs\run_*.log' -ForegroundColor Green
}
Write-Host "`nDone. Report: $report" -ForegroundColor Green
Stop-Transcript | Out-Null
