@echo off
REM Fun Drastic build wrapper for Windows (needs Docker Desktop).
REM   build.bat image        build the toolchain image (once)
REM   build.bat leaf         build the 64-bit hook
REM   build.bat h700   build the 32-bit (armhf) hook
REM   build.bat all          build every hook
REM Linux/macOS use the Makefile (make image / make leaf ...). See docs/BUILDING.md.
setlocal
set HERE=%~dp0
set HERE=%HERE:~0,-1%
set IMAGE=fundrastic-build

if "%1"=="" goto :usage
if /i "%1"=="image" (
  docker build -t %IMAGE% "%HERE%\toolchain"
  goto :eof
)
if /i "%1"=="all" (
  call "%~f0" leaf
  call "%~f0" h700
  call "%~f0" brick
  goto :eof
)
docker run --rm --user root -v "%HERE%:/workspace" %IMAGE% bash /workspace/build.sh %1
goto :eof

:usage
echo Usage: build.bat image ^| leaf ^| h700 ^| brick ^| all
exit /b 1
