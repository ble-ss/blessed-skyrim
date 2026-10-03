@echo off
setlocal

set "ROOT=%~dp0.."
set "DXVK=%ROOT%\dxvk"
set "GLSLANG_BIN=%ROOT%\tools\glslang\bin"
set "PY_SCRIPTS=%USERPROFILE%\AppData\Roaming\Python\Python314\Scripts"
set "OUT=%ROOT%\tools\dxvk-fork\x64"

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo vcvars64.bat failed
    exit /b 1
)

set "PATH=%GLSLANG_BIN%;%PY_SCRIPTS%;%PATH%"

if not exist "%DXVK%\build.msvc" (
    pushd "%DXVK%"
    meson setup build.msvc --backend=ninja --buildtype=release -Ddebug=true -Db_ndebug=true -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false
    if errorlevel 1 (
        popd
        echo meson setup failed
        exit /b 1
    )
    popd
)

pushd "%DXVK%"
ninja -C build.msvc
if errorlevel 1 (
    popd
    echo ninja build failed
    exit /b 1
)
popd

rem "nodeploy": compile only, leave tools\dxvk-fork alone (a benchmark
rem campaign may be using those dlls)
if /i "%~1"=="nodeploy" (
    echo built, not deployed.
    exit /b 0
)

if not exist "%OUT%" mkdir "%OUT%"

for %%F in (d3d11.dll d3d11.pdb dxgi.dll dxgi.pdb) do (
    if exist "%DXVK%\build.msvc\src\d3d11\%%F" copy /y "%DXVK%\build.msvc\src\d3d11\%%F" "%OUT%\" >nul
    if exist "%DXVK%\build.msvc\src\dxgi\%%F" copy /y "%DXVK%\build.msvc\src\dxgi\%%F" "%OUT%\" >nul
)

echo done.
endlocal
