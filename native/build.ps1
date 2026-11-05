param([string]$Target = "all")

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here "build"

function Find-MSVC {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install the VS C++ toolset" }
    $root = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $root) { throw "no VS C++ toolset found" }
    $vcvars = Join-Path $root "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat missing at $vcvars" }
    return $vcvars
}

function Invoke-MSVC([string]$cmdline) {
    $vcvars = Find-MSVC
    $batch = "`"$vcvars`" >nul 2>&1 && cd /d `"$here`" && $cmdline"
    cmd /c $batch | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "MSVC step failed -> $cmdline" }
}

if ($Target -eq "clean") {
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    "clean."
    return
}

New-Item -ItemType Directory -Force -Path $out | Out-Null

$flags = "/nologo /c /W4 /WX /O2 /Oi /Zi /D_CRT_SECURE_NO_WARNINGS /I`"$here`""

$newestHeader = (Get-ChildItem "$here\*.h" | Sort-Object LastWriteTime -Descending |
                 Select-Object -First 1).LastWriteTime

function Build-Obj([string]$s) {
    $obj = Join-Path $out "$s.obj"
    if (-not (Test-Path $obj) -or
        (Get-Item "$here\$s.c").LastWriteTime -gt (Get-Item $obj).LastWriteTime -or
        $newestHeader -gt (Get-Item $obj).LastWriteTime) {
        Write-Host "  cl  $s.c"
        Invoke-MSVC "cl $flags /Fo`"$obj`" `"$here\$s.c`""
    }
    return "`"$obj`""
}

$testObjs = @()
foreach ($s in @("gtav_decode", "gtav_pages", "gtav_pagetest")) { $testObjs += (Build-Obj $s) }
"  link gtav_pagetest.exe"
Invoke-MSVC "link /nologo /DEBUG /OUT:`"$out\gtav_pagetest.exe`" $($testObjs -join ' ')"

# alongside the harness.
$shadowObjs = @()
foreach ($s in @("gtav_decode", "gtav_pages", "gtav_shadow", "gtav_bake")) {
    $shadowObjs += (Build-Obj $s)
}
"  link gtav_bake.exe"
Invoke-MSVC "link /nologo /DEBUG /OUT:`"$out\gtav_bake.exe`" $($shadowObjs -join ' ')"

if ($Target -eq "gtav_bake") {
    "built -> $out\gtav_bake.exe"
    return
}

if ($Target -eq "gtav_pagetest") {
    "built -> $out\gtav_pagetest.exe"
    return
}

if ($Target -eq "acct") {
    $accObj = Join-Path $out "gtav_acc_test.obj"
    if (Test-Path $accObj) {
        if ((Get-Item "$here\gtav.c").LastWriteTime -gt (Get-Item $accObj).LastWriteTime) {
            Remove-Item -Force $accObj
        }
    }
    $accObjs = @()
    foreach ($s in @("gtav_decode", "gtav_pages", "gtav_luaapi", "gtav_mesh", "gtav_acc_test")) {
        $accObjs += (Build-Obj $s)
    }
    "  link gtav_acc_test.exe"
    Invoke-MSVC "link /nologo /DEBUG /OUT:`"$out\gtav_acc_test.exe`" $($accObjs -join ' ')"
    "built -> $out\gtav_acc_test.exe"
    return
}

$mergeObjs = @()
foreach ($s in @("gtav_merge")) { $mergeObjs += (Build-Obj $s) }
"  link gtav_mergetest.exe"
Invoke-MSVC "link /nologo /DEBUG /OUT:`"$out\gtav_mergetest.exe`" $($mergeObjs -join ' ')"

$modObjs = @()
foreach ($s in @("gtav_decode", "gtav_pages", "gtav_luaapi", "gtav_mesh", "gtav_shadow", "gtav_prefetch", "gtav")) {
    $modObjs += (Build-Obj $s)
}
"  link gmcl_gtav_win64.dll"
Invoke-MSVC "link /nologo /DEBUG /DLL /OUT:`"$out\gmcl_gtav_win64.dll`" $($modObjs -join ' ')"

"built -> $out\gtav_pagetest.exe"
"built -> $out\gtav_mergetest.exe"
"built -> $out\gmcl_gtav_win64.dll"

if ($Target -eq "test") {
    $mapdir = 'C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\garrysmod\data\lossantos\lossantos'
    "running harness against $mapdir"
    & "$out\gtav_pagetest.exe" $mapdir scan 45000 -20318.9 -14645.7
    "exit=$LASTEXITCODE"
}
