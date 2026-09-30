<#
Builds tickerhook.dll with MSYS2's MinGW-w64 g++ instead of Visual Studio.

  .\build-mingw.ps1            64-bit, to dist\x64\tickerhook.dll
  .\build-mingw.ps1 -Arch x86  32-bit, to dist\x86\tickerhook.dll

MinHook is compiled from source. By default the copy bundled with spice2x
next to this repository is used; pass -MinHook to point at another checkout.
Everything is linked statically, so the dll only needs Windows' own dlls.
#>
param(
    [ValidateSet("x64", "x86")][string]$Arch = "x64",
    [string]$Msys = "C:\msys64",
    [string]$MinHook = (Join-Path $PSScriptRoot "..\spice2x.github.io\src\spice2x\external\minhook")
)
$ErrorActionPreference = "Stop"

$bin = if ($Arch -eq "x64") { "$Msys\ucrt64\bin" } else { "$Msys\mingw32\bin" }
if (-not (Test-Path "$bin\g++.exe")) { throw "No g++ in $bin, install the MSYS2 toolchain for $Arch" }
if (-not (Test-Path "$MinHook\include\MinHook.h")) { throw "MinHook not found at $MinHook" }
$env:PATH = "$bin;$env:PATH"

$src = Join-Path $PSScriptRoot "TickerHook"
$obj = Join-Path $PSScriptRoot "build-mingw\$Arch"
$out = Join-Path $PSScriptRoot "dist\$Arch"
New-Item -ItemType Directory -Force $obj, $out | Out-Null

function Invoke-Tool([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed" }
}

$objects = @()
foreach ($c in "buffer.c", "hook.c", "trampoline.c", "hde\hde32.c", "hde\hde64.c") {
    $o = Join-Path $obj ("minhook_" + [IO.Path]::GetFileNameWithoutExtension($c) + ".o")
    Invoke-Tool gcc @("-c", "-O2", "-w", "-I$MinHook\include", "$MinHook\src\$c", "-o", $o)
    $objects += $o
}

# dllmain.cpp pulls in SerialServer.cpp itself; pch.cpp is empty apart from the header
$flags = @("-c", "-O2", "-std=c++14", "-Wall", "-Wno-missing-field-initializers", "-Wno-unknown-pragmas",
    "-Wno-conversion-null", "-Wno-unused-variable", "-I$MinHook\include")
foreach ($cpp in "dllmain.cpp", "pch.cpp") {
    $o = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($cpp) + ".o")
    Invoke-Tool g++ ($flags + @("$src\$cpp", "-o", $o))
    $objects += $o
}

$dll = Join-Path $out "tickerhook.dll"
Invoke-Tool g++ (@("-shared", "-static", "-static-libgcc", "-static-libstdc++", "-s", "-o", $dll) + $objects + @("-lpsapi"))
Write-Host "built $dll ($((Get-Item $dll).Length) bytes)"

# Test tool for the relay board, see docs\relay-board.md
$test = Join-Path $out "relaytest.exe"
Invoke-Tool g++ (@("-O2", "-std=c++14", "-Wall", "-Wno-missing-field-initializers", "-I$MinHook\include", "-static", "-s",
    (Join-Path $PSScriptRoot "RelayTest\relaytest.cpp"), "-o", $test))
Write-Host "built $test ($((Get-Item $test).Length) bytes)"
