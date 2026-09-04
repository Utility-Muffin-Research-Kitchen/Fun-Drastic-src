@echo off
REM Fun Drastic package wrapper for Windows (needs Docker Desktop).
REM   pack.bat leaf         assemble the Leaf package
REM   pack.bat h700   assemble the h700 NDS.pak
REM Build the hook first (build.bat <target>). Supply DraStic under emulator\
REM (see docs/BUILDING.md). Linux/macOS use `make pack-<target>` / ./pack.sh.
setlocal
set HERE=%~dp0
set HERE=%HERE:~0,-1%
if "%1"=="" ( echo Usage: pack.bat leaf ^| h700 & exit /b 1 )
docker run --rm --user root -v "%HERE%:/workspace" fundrastic-build bash /workspace/pack.sh %1
goto :eof
