@echo off
setlocal enabledelayedexpansion

:: Switch to the directory where this bat file is located
cd /d "%~dp0"

echo ===================================================
echo   ESP32-C3 Firmware Flashing Script (BareTcl)
echo ===================================================
echo.

:: 1. Define potential esptool.exe paths
:: Your customized directory on the root of the drive (e.g., Z:\esptool-windows-amd64)
set "ESPTOOL_EXE=%~d0\esptool-windows-amd64\esptool.exe"

:: Fallback to the local directory (if unzipped or needs to be unzipped)
if not exist "!ESPTOOL_EXE!" (
    set "ESPTOOL_EXE=%~dp0esptool-v4.11.0-windows-amd64\esptool.exe"
    
    if not exist "!ESPTOOL_EXE!" (
        if exist "%~dp0esptool-v4.11.0-windows-amd64.zip" (
            echo [INFO] Extracting local esptool zip file...
            powershell -Command "Expand-Archive -Path '%~dp0esptool-v4.11.0-windows-amd64.zip' -DestinationPath '%~dp0esptool-v4.11.0-windows-amd64' -Force"
            if !errorlevel! neq 0 (
                echo [ERROR] Failed to extract local esptool.
                pause
                exit /b 1
            )
            echo [INFO] Extraction completed.
        ) else (
            echo [ERROR] Cannot find esptool.exe at "%~d0\esptool-windows-amd64\esptool.exe" or local folder.
            echo Please check the installation path.
            pause
            exit /b 1
        )
    )
)

echo [INFO] Using esptool: "!ESPTOOL_EXE!"

:: 2. Load COM port from flashcom.txt
set "COM_FILE=%~dp0flashcom.txt"
set "COM_PORT=COM3"

if exist "%COM_FILE%" (
    set /p COM_PORT=<"%COM_FILE%"
    :: Trim spaces if any
    set "COM_PORT=!COM_PORT: =!"
) else (
    echo COM3>"%COM_FILE%"
)

echo [INFO] Current configured port: !COM_PORT!
set /p "INPUT_PORT=Press ENTER to use !COM_PORT!, or type a new COM port (e.g. COM4): "

if not "%INPUT_PORT%"=="" (
    set "COM_PORT=%INPUT_PORT%"
    echo !COM_PORT!>"%COM_FILE%"
    echo [INFO] Updated flashcom.txt to !COM_PORT!
)

:: 3. Check if build files exist
if not exist "build\bootloader\bootloader.bin" (
    echo [ERROR] Cannot find build\bootloader\bootloader.bin. Make sure you have compiled the project.
    pause
    exit /b 1
)

:: 4. Run esptool
echo.
echo [INFO] Flashing firmware to !COM_PORT! ...
echo.

"!ESPTOOL_EXE!" -p !COM_PORT! -b 460800 --before default_reset --after hard_reset --chip esp32c3 write_flash --flash_mode dio --flash_size 2MB --flash_freq 80m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\ESP32_ports.bin

if !errorlevel! equ 0 (
    echo.
    echo ===================================================
    echo   Flashing Completed Successfully!
    echo ===================================================
) else (
    echo.
    echo [ERROR] Flashing failed. Please check the COM port, USB connection, or driver.
)

pause
