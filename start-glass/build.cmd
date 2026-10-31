@echo off
rem Builds start-glass.dll and start-glass.exe into .\out with the Visual Studio 2022 C++ tools:
rem %VCVARS% (their vcvars64.bat, which build.ps1 finds), else the Build Tools' default place.
setlocal
cd /d "%~dp0"
if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>nul || exit /b 1
if not exist out mkdir out
set FLAGS=/nologo /std:c++17 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /Fo:out\
cl %FLAGS% /LD tap.cpp /link /DEF:tap.def /OUT:out\start-glass.dll windowsapp.lib d2d1.lib dwmapi.lib ole32.lib oleaut32.lib user32.lib || exit /b 1
cl %FLAGS% inject.cpp /link /SUBSYSTEM:WINDOWS /OUT:out\start-glass.exe ole32.lib user32.lib || exit /b 1
del out\*.obj out\*.exp out\*.lib 2>nul
echo built: out\start-glass.dll out\start-glass.exe
