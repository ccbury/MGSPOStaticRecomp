@echo off
rem Double-click launcher. Runs setup on first use, then starts the game.
rem Looks for mgspo-launcher.exe in a few usual build folders; pass an .iso path to change the disc.
setlocal
set "HERE=%~dp0"
for %%D in ("%HERE%" "%HERE%build\" "%HERE%out\build\") do (
  if exist "%%~Dmgspo-launcher.exe" (
    "%%~Dmgspo-launcher.exe" %*
    exit /b %errorlevel%
  )
)
echo mgspo-launcher.exe was not found. Build the project first:
echo   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^&^& cmake --build build --target mgspo-launcher
pause
exit /b 1
