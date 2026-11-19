@echo off
rem Build a CD-i static recompilation.
rem Requirements: Python 3, CMake 3.19+, Visual Studio 2022 or its Build Tools
rem (C++ workload), and the game's disc image (.bin/.cue).
rem The disc image is found through RECOMP_ROM (path to the .cue or .bin),
rem then rom= in rom.cfg, then rom\*.bin.  Per-game settings: game.json
rem Set PYTHON to use a specific interpreter (default: python).
setlocal
cd /d "%~dp0"
if not defined PYTHON set PYTHON=python

for /f "delims=" %%n in ('call "%PYTHON%" -c "import json;print(json.load(open('game.json')).get('exe_name','cdigame'))"') do set EXE=%%n
if "%EXE%"=="" goto :error

echo [1/3] Recompiling 68000 code to C...
"%PYTHON%" tools\gen.py || goto :error

echo [2/3] Configuring...
cmake -S . -B build\cmake -G "Visual Studio 17 2022" -A x64 > nul || goto :error

echo [3/3] Compiling...
cmake --build build\cmake --config Release --target game --parallel || goto :error

copy /y build\cmake\Release\%EXE%.exe %EXE%.exe > nul || goto :error
echo.
echo Done. Run %EXE%.exe to play.
exit /b 0

:error
echo Build failed.
exit /b 1
