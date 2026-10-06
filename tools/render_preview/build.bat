@echo off
REM Adjust this path if Visual Studio is installed somewhere else, or to a
REM different edition (Professional/Enterprise) than Community.
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo Failed to set up MSVC environment - is Visual Studio installed at the path above?
  exit /b 1
)
REM vcvars64.bat prints "'vswhere.exe' is not recognized" on this machine
REM (it isn't on PATH) - harmless, vcvars64 falls back fine without it.
cl.exe /nologo /EHsc /std:c++17 /utf-8 /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS render_preview.cpp /Fe:render_preview.exe
