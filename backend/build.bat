@echo off
REM Build script for Windows (MSVC)
REM Prerequisites: cmake, Visual Studio, vcpkg
REM
REM vcpkg setup (one-time):
REM   git clone https://github.com/microsoft/vcpkg
REM   .\vcpkg\bootstrap-vcpkg.bat
REM   .\vcpkg\vcpkg install grpc:x64-windows
REM
REM Then set VCPKG_ROOT to your vcpkg directory.

setlocal

set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build

if defined VCPKG_ROOT (
  set TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
) else (
  set TOOLCHAIN=
  echo [warn] VCPKG_ROOT not set. Make sure grpc is findable by CMake.
)

echo =^> Configuring...
cmake -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -A x64 ^
  %TOOLCHAIN%

if errorlevel 1 (
  echo [error] CMake configure failed.
  exit /b 1
)

echo =^> Building...
cmake --build "%BUILD_DIR%" --config Release --parallel

if errorlevel 1 (
  echo [error] Build failed.
  exit /b 1
)

echo.
echo Build complete. Binaries:
echo   %BUILD_DIR%\Release\renderer_server.exe
echo   %BUILD_DIR%\Release\renderer_client.exe
echo.
echo Run server:  %BUILD_DIR%\Release\renderer_server.exe
echo Run client:  %BUILD_DIR%\Release\renderer_client.exe
