@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

set "APP_NAME=PADCamResolutionProbe"
set "EXE_NAME=%APP_NAME%.exe"
set "BUILD_DIR=%SCRIPT_DIR%\build\Desktop_Qt_5_15_2_MSVC2019_64bit-Release"
set "EXE_PATH=%BUILD_DIR%\%EXE_NAME%"
set "DEPLOY_ROOT=%SCRIPT_DIR%\deploy"
set "DEPLOY_DIR=%DEPLOY_ROOT%\%APP_NAME%"
set "ZIP_PATH=%DEPLOY_ROOT%\%APP_NAME%_release.zip"
set "WINDEPLOYQT="

echo.
echo [1/5] Checking build output...
if not exist "%EXE_PATH%" (
    echo ERROR: Executable not found:
    echo   %EXE_PATH%
    echo Please build the Release target first in Qt Creator.
    exit /b 1
)

echo.
echo [2/5] Locating windeployqt...
for /f "delims=" %%I in ('where windeployqt.exe 2^>nul') do (
    set "WINDEPLOYQT=%%~fI"
    goto :windeployqt_found
)

if exist "C:\Qt\5.15.2\msvc2019_64\bin\windeployqt.exe" (
    set "WINDEPLOYQT=C:\Qt\5.15.2\msvc2019_64\bin\windeployqt.exe"
    goto :windeployqt_found
)

echo ERROR: windeployqt.exe was not found.
echo Please add Qt bin to PATH or install Qt 5.15.2 MSVC2019 64bit.
exit /b 1

:windeployqt_found
echo Using:
echo   %WINDEPLOYQT%

echo.
echo [3/5] Preparing deploy folder...
if not exist "%DEPLOY_ROOT%" mkdir "%DEPLOY_ROOT%"
if exist "%DEPLOY_DIR%" rmdir /s /q "%DEPLOY_DIR%"
mkdir "%DEPLOY_DIR%"
copy /y "%EXE_PATH%" "%DEPLOY_DIR%\%EXE_NAME%" >nul

echo.
echo [4/5] Running windeployqt...
"%WINDEPLOYQT%" --release --compiler-runtime --dir "%DEPLOY_DIR%" "%DEPLOY_DIR%\%EXE_NAME%"
if errorlevel 1 (
    echo ERROR: windeployqt failed.
    exit /b 1
)

echo.
echo [5/5] Creating zip package...
if exist "%ZIP_PATH%" del /f /q "%ZIP_PATH%" >nul 2>nul
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%DEPLOY_DIR%\*' -DestinationPath '%ZIP_PATH%' -Force"
if errorlevel 1 (
    echo WARNING: Zip creation failed, but deploy folder is ready.
    echo Deploy folder:
    echo   %DEPLOY_DIR%
    exit /b 0
)

echo.
echo Package completed successfully.
echo Deploy folder:
echo   %DEPLOY_DIR%
echo Zip package:
echo   %ZIP_PATH%
echo.
echo Copy the whole deploy folder or the zip file to the PAD.
exit /b 0
