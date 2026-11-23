@echo off
rem Builds out\Juice.exe with Visual Studio (static runtime, nothing extra to install).
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist out mkdir out
rc /nologo /i res /fo out\juice.res res\juice.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /utf-8 /permissive- /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Foout\ src\*.cpp out\juice.res /Feout\Juice.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MANIFEST:NO setupapi.lib hid.lib cfgmgr32.lib runtimeobject.lib ole32.lib shell32.lib user32.lib gdi32.lib dwmapi.lib advapi32.lib shlwapi.lib gdiplus.lib uuid.lib
exit /b %errorlevel%
