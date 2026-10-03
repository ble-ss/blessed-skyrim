@echo off
rem blessed: dxvk build via llvm-mingw (clang, ucrt) instead of msvc.
rem args: "noprobe" selects the -Dblessed_probe=false
rem variant; "nodeploy" compiles only, does not copy dlls into tools\.
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "DXVK=%ROOT%\dxvk"
set "TOOLCHAIN=%ROOT%\tools\llvm-mingw-20260922-ucrt-x86_64\bin"
set "GLSLANG_BIN=%ROOT%\tools\glslang\bin"
set "PY_SCRIPTS=%USERPROFILE%\AppData\Roaming\Python\Python314\Scripts"
set "NATIVEFILE=%ROOT%\tools\build-win64-llvm-mingw-native.ini"

set "VARIANT="
set "NODEPLOY="
for %%A in (%*) do (
    if /i "%%~A"=="noprobe" set "VARIANT=noprobe"
    if /i "%%~A"=="nodeploy" set "NODEPLOY=1"
)

if defined VARIANT (
    set "BUILDDIR=build.mingw-noprobe"
    set "OUT=%ROOT%\tools\dxvk-fork-mingw-noprobe\x64"
    set "PROBEOPT=-Dblessed_probe=false"
) else (
    set "BUILDDIR=build.mingw"
    set "OUT=%ROOT%\tools\dxvk-fork-mingw\x64"
    set "PROBEOPT=-Dblessed_probe=true"
)

set "PATH=%TOOLCHAIN%;%GLSLANG_BIN%;%PY_SCRIPTS%;%PATH%"

if not exist "%DXVK%\%BUILDDIR%" (
    pushd "%DXVK%"
    meson setup %BUILDDIR% --backend=ninja --buildtype=release --strip -Db_ndebug=true -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false %PROBEOPT% --native-file "%NATIVEFILE%"
    if errorlevel 1 (
        popd
        echo meson setup failed
        exit /b 1
    )
    popd
)

pushd "%DXVK%"
ninja -C %BUILDDIR%
if errorlevel 1 (
    popd
    echo ninja build failed
    exit /b 1
)
popd

rem "nodeploy": compile only, leave tools\dxvk-fork-mingw* alone (a benchmark
rem campaign may be using those dlls)
if defined NODEPLOY (
    echo built, not deployed.
    exit /b 0
)

if not exist "%OUT%" mkdir "%OUT%"

for %%F in (d3d11.dll dxgi.dll) do (
    if exist "%DXVK%\%BUILDDIR%\src\d3d11\%%F" copy /y "%DXVK%\%BUILDDIR%\src\d3d11\%%F" "%OUT%\" >nul
    if exist "%DXVK%\%BUILDDIR%\src\dxgi\%%F" copy /y "%DXVK%\%BUILDDIR%\src\dxgi\%%F" "%OUT%\" >nul
)

echo done.
endlocal
