@echo off
REM builds skse\skybench into tools\skybench-plugin\skybench.dll (+ pdb).
REM does not copy into the game folder - the harness stages it.
setlocal

set ROOT=%~dp0..
set BUILD_DIR=%ROOT%\skse\skybench\build
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist %VCVARS% (
	echo vcvars64.bat not found at %VCVARS%
	exit /b 1
)

call %VCVARS% || exit /b 1

set PATH=%PATH%;%USERPROFILE%\AppData\Roaming\Python\Python314\Scripts

cmake -S "%ROOT%\skse\skybench" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
cmake --build "%BUILD_DIR%" || exit /b 1

echo.
echo built: %ROOT%\tools\skybench-plugin\skybench.dll
endlocal
