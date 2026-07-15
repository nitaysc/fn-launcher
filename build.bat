@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "C:\Users\ganga\OneDrive\913C~1\AIVSFO~1\FN-LAU~1"
echo === Compiling Launcher.exe ===
cl /nologo /O2 /MT /W3 /EHsc launcher.cpp /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" user32.lib kernel32.lib gdi32.lib comctl32.lib urlmon.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib shlwapi.lib winhttp.lib /OUT:Launcher.exe
if %errorlevel% equ 0 ( echo BUILD SUCCESS && dir Launcher.exe ) else ( echo BUILD FAILED )
pause
