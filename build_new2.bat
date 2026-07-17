@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "%~dp0"
echo === Compiling Launcher_new2.exe ===
cl /nologo /O2 /MT /W3 /EHsc launcher.cpp /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" user32.lib kernel32.lib gdi32.lib comctl32.lib urlmon.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib shlwapi.lib winhttp.lib dwmapi.lib msimg32.lib gdiplus.lib /OUT:Launcher_new2.exe
if %errorlevel% equ 0 ( echo BUILD SUCCESS ) else ( echo BUILD FAILED )