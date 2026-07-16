@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "C:\Users\ganga\OneDrive\913C~1\AIVSFO~1\AI vs Fortnite\FortniteESP"
call build_cl.bat
pause
