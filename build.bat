@echo off
rem Builds release\dinput8.dll with Zig (https://ziglang.org) - 32-bit Windows target.
cd /d "%~dp0src"
zig cc -target x86-windows-gnu -shared -O2 -o ..\release\dinput8.dll squadmod.c dinput8.def -lwsock32 -luser32
if errorlevel 1 (echo Build failed & exit /b 1)
del /q ..\release\dinput8.pdb ..\release\dinput8.lib ..\release\squadmod.lib 2>nul
echo Built release\dinput8.dll
