param([string]$Target = "all")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here "build"

function Find-MSVC {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $root = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $root) { throw "no VS C++ toolset found" }
    return (Join-Path $root "VC\Auxiliary\Build\vcvars64.bat")
}
function Invoke-MSVC([string]$cmdline) {
    $batch = "`"$(Find-MSVC)`" >nul 2>&1 && cd /d `"$here`" && $cmdline"
    cmd /c $batch | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "MSVC step failed -> $cmdline" }
}

if ($Target -eq "clean") { if (Test-Path $out) { Remove-Item -Recurse -Force $out }; "clean."; return }
New-Item -ItemType Directory -Force -Path $out | Out-Null

$flags = "/nologo /c /W4 /WX /O2 /Oi /GL /Zi /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I`"$here`""
$core = @("lossantos_common", "lossantos_crypto", "lossantos_inflate", "lossantos_rpf", "lossantos_rsc", "lossantos_ytd", "lossantos_ydr", "lossantos_ymap", "lossantos_ybn", "lossantos_util", "lossantos_geo", "lossantos_tex", "lossantos_block", "lossantos_out", "lossantos_stage", "lossantos_clip", "lossantos_map", "lossantos_tile", "lossantos_water", "lossantos_beach", "lossantos_slod", "lossantos_vtf", "lossantos_gma", "lossantos_content", "lossantos_keys", "lossantos_radio")
$newestHeader = (Get-ChildItem "$here\*.h" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime

function Build-Obj([string]$s) {
    $obj = Join-Path $out "$s.obj"
    if (-not (Test-Path $obj) -or (Get-Item "$here\$s.c").LastWriteTime -gt (Get-Item $obj).LastWriteTime -or
        $newestHeader -gt (Get-Item $obj).LastWriteTime) {
        Write-Host "  cl  $s.c"
        Invoke-MSVC "cl $flags /Fo`"$obj`" `"$here\$s.c`""
    }
    return "`"$obj`""
}

$coreObjs = @(); foreach ($s in $core) { $coreObjs += (Build-Obj $s) }
$testObj = Build-Obj "lossantos_test"
"  link lossantos_test.exe"
Invoke-MSVC "link /nologo /LTCG /DEBUG /OUT:`"$out\lossantos_test.exe`" $($coreObjs -join ' ') $testObj"
"built -> $out\lossantos_test.exe"

$dllObj = Build-Obj "lossantos_install"
"  link lossantos_install.dll"
Invoke-MSVC "link /nologo /LTCG /DEBUG /DLL /OUT:`"$out\lossantos_install.dll`" $($coreObjs -join ' ') $dllObj"
"built -> $out\lossantos_install.dll"

$cliObj = Build-Obj "lossantos_cli"
"  link lossantos_cli.exe"
Invoke-MSVC "link /nologo /LTCG /DEBUG /OUT:`"$out\lossantos_cli.exe`" $cliObj"
"built -> $out\lossantos_cli.exe"

$native = Split-Path -Parent $here
$sflags = "/nologo /c /W3 /O2 /D_CRT_SECURE_NO_WARNINGS /I`"$native`""
$sobjs = @()
foreach ($s in @("gtav_decode", "gtav_pages", "gtav_shadow", "gtav_bake")) {
    $obj = Join-Path $out "sh_$s.obj"
    if (-not (Test-Path $obj) -or (Get-Item "$native\$s.c").LastWriteTime -gt (Get-Item $obj).LastWriteTime) {
        Write-Host "  cl  $s.c (shadow)"
        Invoke-MSVC "cl $sflags /Fo`"$obj`" `"$native\$s.c`""
    }
    $sobjs += "`"$obj`""
}
"  link gtav_bake.exe"
Invoke-MSVC "link /nologo /OUT:`"$out\gtav_bake.exe`" $($sobjs -join ' ')"
"built -> $out\gtav_bake.exe"

$payload = Join-Path $out "payload"
New-Item -ItemType Directory -Force $payload | Out-Null
Copy-Item (Join-Path $native "build\gmcl_gtav_win64.dll") $payload -Force
Copy-Item (Join-Path $here "payload\*") $payload -Exclude *.vmf -Force
"built -> $payload"
