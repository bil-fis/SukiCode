@echo off
REM SukiCode Integration Tests - Windows
REM All build artifacts go to moduleTest\build\

setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set PROJECT_DIR=%SCRIPT_DIR%..
set BUILD_DIR=%SCRIPT_DIR%build
set SUKIC=%PROJECT_DIR%\build\bin\sukic.exe

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

set PASS=0
set FAIL=0

echo === SukiCode Integration Tests ===
echo.

echo --- Basic ---
call :run_test hello_world "%SCRIPT_DIR%\integration\hello_world.suki" "Hello, SukiCode!"
call :run_test fibonacci "%SCRIPT_DIR%\integration\fibonacci.suki" "55"
call :run_test generic "%SCRIPT_DIR%\integration\generic.suki" "42"

echo.
echo --- Compile Only ---
for %%f in ("%SCRIPT_DIR%\lexer\*.suki" "%SCRIPT_DIR%\parser\*.suki") do (
    set "name=%%~nf"
    set "exe=%BUILD_DIR%\%%~nf.exe"
    echo -n   %%~nf ...
    "%SUKIC%" -o "!exe!" "%%f" >nul 2>&1
    if !errorlevel! equ 0 (
        echo  PASS
        set /a PASS+=1
    ) else (
        echo  FAIL
        set /a FAIL+=1
    )
)

echo.
echo === Results: %PASS% passed, %FAIL% failed ===
exit /b %FAIL%

:run_test
set "name=%~1"
set "src=%~2"
set "expected=%~3"
set "exe=%BUILD_DIR%\%name%.exe"

echo -n   %name% ...
"%SUKIC%" -o "%exe%" "%src%" >nul 2>&1
if !errorlevel! neq 0 (
    echo  FAIL (compile)
    set /a FAIL+=1
    goto :eof
)

for /f "delims=" %%i in ('"%exe%" 2^>nul') do set "output=%%i"
if "%output%"=="%expected%" (
    echo  PASS
    set /a PASS+=1
) else (
    echo  FAIL (expected '%expected%', got '%output%')
    set /a FAIL+=1
)
goto :eof
