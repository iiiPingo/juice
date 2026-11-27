@echo off
rem Builds and runs the status.txt tests with Visual Studio. Exit code 0 = all passed.
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist out mkdir out
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /utf-8 /permissive- /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Foout\ status_test.cpp ..\src\status.cpp ..\src\util.cpp /Feout\status_test.exe /link /SUBSYSTEM:CONSOLE shell32.lib ole32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
out\status_test.exe
exit /b %errorlevel%
