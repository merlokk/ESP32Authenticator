@echo off
setlocal

rem Build and run the host tests (no board needed).
rem   host_test\run.cmd            all suites
rem   host_test\run.cmd config     suites whose name contains "config"
rem Uses MSVC Build Tools, and the CMake/Ninja that ESP-IDF ships.
rem Kept ASCII and CRLF: cmd.exe misparses UTF-8 punctuation.

set "VCVARSALL=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
set "CMAKE=C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe"
set "NINJA=C:\Espressif\tools\ninja\1.12.1\ninja.exe"
if "%IDF_PATH%"=="" set "IDF_PATH=E:\esp\v6.0.2\esp-idf"

rem goto, not a parenthesised if: the path holds "(x86)".
if not exist "%VCVARSALL%" goto :novs

call "%VCVARSALL%" x64 >nul || exit /b 1

pushd "%~dp0"
"%CMAKE%" -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DIDF_PATH="%IDF_PATH%" || goto :fail
"%CMAKE%" --build build || goto :fail
build\host_test.exe %* || goto :fail
popd
exit /b 0

:fail
popd
exit /b 1

:novs
echo Visual Studio Build Tools not found at:
echo   %VCVARSALL%
exit /b 1
