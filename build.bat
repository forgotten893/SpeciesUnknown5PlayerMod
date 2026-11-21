@echo off
rem Builds build\version.dll (x64) with the MSVC toolchain.
setlocal
set VCVARS=
for %%P in (
  "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) do if exist %%P if not defined VCVARS set VCVARS=%%P
if not defined VCVARS ( echo vcvars64.bat not found & exit /b 1 )
call %VCVARS% >nul || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build
ml64 /nologo /c /Fobuild\version_proxy.obj src\version_proxy.asm || exit /b 1
cl /nologo /c /O2 /MT /EHsc /W3 /std:c++17 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /Fobuild\ src\dllmain.cpp src\fiveplayer.cpp || exit /b 1
link /nologo /DLL /OUT:build\version.dll /DEF:src\version.def ^
   build\dllmain.obj build\fiveplayer.obj build\version_proxy.obj kernel32.lib user32.lib || exit /b 1
echo Built build\version.dll
