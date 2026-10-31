@echo off
rem Builds taskbar-dock.dll into .\out with the Visual Studio 2022 C++ tools (fxc comes with the
rem Windows SDK): %VCVARS% (their vcvars64.bat, which build.ps1 finds), else the Build Tools' default place.
setlocal
cd /d "%~dp0"
if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>nul || exit /b 1
if not exist out mkdir out
fxc /nologo /T vs_4_0 /E FullScreen /Vn g_fullScreen /Fh out\glass_vs.h glass.hlsl >nul || exit /b 1
fxc /nologo /T ps_4_0 /E Glass /Vn g_glass /Fh out\glass_ps.h glass.hlsl >nul || exit /b 1
fxc /nologo /T ps_4_0 /E GhostPicture /Vn g_ghost /Fh out\ghost_ps.h glass.hlsl >nul || exit /b 1
set FLAGS=/nologo /std:c++17 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /Fo:out\
cl %FLAGS% /LD dock.cpp /link /DEF:dock.def /OUT:out\taskbar-dock.dll windowsapp.lib CoreMessaging.lib d3d11.lib dwmapi.lib magnification.lib user32.lib shell32.lib || exit /b 1
del out\*.obj out\*.exp out\*.lib 2>nul
echo built: out\taskbar-dock.dll
