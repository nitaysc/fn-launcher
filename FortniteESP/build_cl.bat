@echo off
setlocal

echo ========================================
echo   Building Fortnite ESP (cl.exe)
echo ========================================
echo.

REM ----------------------------------------------------------------
REM  Setup x64 Visual Studio environment
REM ----------------------------------------------------------------
echo [+] Setting up Visual Studio 2025 x64 environment...
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [-] vcvars64.bat failed! Make sure VS 2025 is installed with C++ tools.
    pause
    exit /b 1
)

REM ----------------------------------------------------------------
REM  Create output directories
REM ----------------------------------------------------------------
if not exist "bin\Release" mkdir bin\Release
if not exist "obj\Release" mkdir obj\Release

REM ----------------------------------------------------------------
REM  Compile ImGui source files
REM ----------------------------------------------------------------
echo [+] Compiling ImGui library...

set IMGUI_DIR=..\imgui-master
set OBJ_DIR=obj\Release

cl /nologo /c /O2 /MT /W3 /std:c++17 /EHsc ^
    /D"NDEBUG" /D"_CONSOLE" /D"_UNICODE" /D"UNICODE" /D"_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING" ^
    /I"%IMGUI_DIR%" /I"%IMGUI_DIR%\backends" /I"..\Ultra Detected Driver" ^
    /Fo"%OBJ_DIR%\imgui.obj" "%IMGUI_DIR%\imgui.cpp"

if errorlevel 1 ( echo [-] imgui.cpp compilation failed & pause & exit /b 1 )

cl /nologo /c /O2 /MT /W3 /std:c++17 /EHsc ^
    /D"NDEBUG" /D"_CONSOLE" /D"_UNICODE" /D"UNICODE" /D"_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING" ^
    /I"%IMGUI_DIR%" /I"%IMGUI_DIR%\backends" ^
    /Fo"%OBJ_DIR%\imgui_demo.obj" "%IMGUI_DIR%\imgui_demo.cpp"

if errorlevel 1 ( echo [-] imgui_demo.cpp compilation failed & pause & exit /b 1 )

cl /nologo /c /O2 /MT /W3 /std:c++17 /EHsc ^
    /D"NDEBUG" /D"_CONSOLE" /D"_UNICODE" /D"UNICODE" /D"_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING" ^
    /I"%IMGUI_DIR%" /I"%IMGUI_DIR%\backends" ^
    /Fo"%OBJ_DIR%\imgui_draw.obj" "%IMGUI_DIR%\imgui_draw.cpp"

if errorlevel 1 ( echo [-] imgui_draw.cpp compilation failed & pause & exit /b 1 )

cl /nologo /c /O2 /MT /W3 /std:c++17 /EHsc ^
    /D"NDEBUG" /D"_CONSOLE" /D"_UNICODE" /D"UNICODE" /D"_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING" ^
    /I"%IMGUI_DIR%" /I"%IMGUI_DIR%\backends" ^
    /Fo"%OBJ_DIR%\imgui_tables.obj" "%IMGUI_DIR%\imgui_tables.cpp"

if errorlevel 1 ( echo [-] imgui_tables.cpp compilation failed & pause & exit /b 1 )

cl /nologo /c /O2 /MT /W3 /std:c++17 /EHsc ^
    /D"NDEBUG" /D"_CONSOLE" /D"_UNICODE" /D"UNICODE" /D"_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING" ^
    /I"%IMGUI_DIR%" /I"%IMGUI_DIR%\backends" ^
    /Fo"%OBJ_DIR%\imgui_widgets.obj" "%IMGUI_DIR%\imgui_widgets.cpp"

if errorlevel 1 ( echo [-] imgui_widgets.cpp compilation failed & pause & exit /b 1 )

REM ----------------------------------------------------------------
REM  Compile ImGui backends (Win32 + DX11)
REM ----------------------------------------------------------------
echo [+] Compiling ImGui backends...

cl /nologo /c /O2 /MT /W3 /std:c++17 /EHsc ^
    /D"NDEBUG" /D"_CONSOLE" /D"_UNICODE" /D"UNICODE" /D"_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING" ^
    /I"%IMGUI_DIR%" /I"%IMGUI_DIR%\backends" ^
    /Fo"%OBJ_DIR%\imgui_impl_win32.obj" "%IMGUI_DIR%\backends\imgui_impl_win32.cpp"

if errorlevel 1 ( echo [-] imgui_impl_win32.cpp compilation failed & pause & exit /b 1 )

cl /nologo /c /O2 /MT /W3 /std:c++17 /EHsc ^
    /D"NDEBUG" /D"_CONSOLE" /D"_UNICODE" /D"UNICODE" /D"_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING" ^
    /I"%IMGUI_DIR%" /I"%IMGUI_DIR%\backends" ^
    /Fo"%OBJ_DIR%\imgui_impl_dx11.obj" "%IMGUI_DIR%\backends\imgui_impl_dx11.cpp"

if errorlevel 1 ( echo [-] imgui_impl_dx11.cpp compilation failed & pause & exit /b 1 )

REM ----------------------------------------------------------------
REM  Compile main.cpp
REM ----------------------------------------------------------------
echo [+] Compiling main.cpp...

cl /nologo /c /O2 /MT /W3 /std:c++17 /EHsc ^
    /D"NDEBUG" /D"_CONSOLE" /D"_UNICODE" /D"UNICODE" ^
    /I"%IMGUI_DIR%" /I"%IMGUI_DIR%\backends" /I"..\Ultra Detected Driver" ^
    /Fo"%OBJ_DIR%\main.obj" "main.cpp"

if errorlevel 1 ( echo [-] main.cpp compilation failed & pause & exit /b 1 )

REM ----------------------------------------------------------------
REM  Link everything together
REM ----------------------------------------------------------------
echo [+] Linking FortniteESP.exe...

link /nologo /OUT:"bin\Release\FortniteESP.exe" ^
    /SUBSYSTEM:CONSOLE ^
    /MACHINE:X64 ^
    /OPT:REF /OPT:ICF ^
    "%OBJ_DIR%\main.obj" ^
    "%OBJ_DIR%\imgui.obj" ^
    "%OBJ_DIR%\imgui_demo.obj" ^
    "%OBJ_DIR%\imgui_draw.obj" ^
    "%OBJ_DIR%\imgui_tables.obj" ^
    "%OBJ_DIR%\imgui_widgets.obj" ^
    "%OBJ_DIR%\imgui_impl_win32.obj" ^
    "%OBJ_DIR%\imgui_impl_dx11.obj" ^
    d3d11.lib psapi.lib advapi32.lib kernel32.lib user32.lib gdi32.lib

if errorlevel 1 ( 
    echo [-] Linking failed!
    pause
    exit /b 1
)

REM ----------------------------------------------------------------
REM  Cleanup intermediate files
REM ----------------------------------------------------------------
echo [+] Cleaning up...
del /q "%OBJ_DIR%\*.obj" 2>nul

echo.
echo =========================================================
echo  BUILD SUCCESSFUL!
echo =========================================================
echo  Executable: bin\Release\FortniteESP.exe
echo  Run as Administrator to access the driver
echo =========================================================
echo.

pause
