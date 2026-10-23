@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   ShotStopper - Smart Bring-Up Script
echo ============================================

rem --- Auto-detect bootloader file ---
for %%F in (shotStopper_bootloader_*.bin) do (
    set BOOTLOADER_FILE=%%F
)
if not defined BOOTLOADER_FILE (
    echo ERROR: No bootloader file found matching shotStopper_bootloader_*.bin
    pause
    exit /b 1
)

echo Found bootloader file: %BOOTLOADER_FILE%
echo.

rem --- Auto-detect partition table file ---
for %%F in (shotStopper_partitions_*.bin) do (
    set PARTITIONS_FILE=%%F
)
if not defined PARTITIONS_FILE (
    echo ERROR: No partition table file found matching shotStopper_partitions_*.bin
    pause
    exit /b 1
)

echo Found partition table: %PARTITIONS_FILE%
echo.

rem --- List all app files ---
set COUNT=0
for %%F in (shotStopper_app_*.bin) do (
    set /a COUNT+=1
    set APP_FILE_!COUNT!=%%F

    rem --- Determine label based on filename ending ---
    set LABEL_!COUNT!=

    echo %%F | findstr /i "_mom0_reed0.bin" >nul && set LABEL_!COUNT!=Micra/Mini
    echo %%F | findstr /i "_mom1_reed1.bin" >nul && set LABEL_!COUNT!=GS3
    echo %%F | findstr /i "_mom1_reed0.bin" >nul && set LABEL_!COUNT!=Silvia
    echo %%F | findstr /i "_mom0_reed1.bin" >nul && set LABEL_!COUNT!=Stone
)

if %COUNT%==0 (
    echo ERROR: No app files found matching shotStopper_app_*.bin
    pause
    exit /b 1
)

echo Available application files:
for /l %%I in (1,1,%COUNT%) do (
    set FILE=!APP_FILE_%%I!
    set LABEL=!LABEL_%%I!

    if defined LABEL (
        echo   %%I^) !FILE!   [!LABEL!]
    ) else (
        echo   %%I^) !FILE!
    )
)

echo.
set /p USER_CHOICE=Enter the number of the app file to flash: 

if %USER_CHOICE% GTR %COUNT% (
    echo Invalid selection.
    pause
    exit /b 1
)

set APP_FILE=!APP_FILE_%USER_CHOICE%!

echo.
echo Selected app file: %APP_FILE%
echo.

:FLASH_LOOP
echo ============================================
echo   READY TO FLASH BOARD
echo ============================================
echo.
echo Make sure the board is in DOWNLOAD MODE:
echo   1) Hold BOOT
echo   2) Plug in USB
echo   3) Release BOOT
echo.
pause

echo.
echo Erasing flash...
python -m esptool erase-flash
if %errorlevel% neq 0 (
    echo ERROR: erase-flash failed.
    pause
    goto FLASH_LOOP
)

echo.
echo Flashing bootloader + partitions + app...
python -m esptool write-flash ^
    0x0 "%BOOTLOADER_FILE%" ^
    0x8000 "%PARTITIONS_FILE%" ^
    0x10000 "%APP_FILE%"
if %errorlevel% neq 0 (
    echo ERROR: write-flash failed.
    pause
    goto FLASH_LOOP
)

echo.
echo ============================================
echo   Flashing complete!
echo   Unplug this board.
echo   Plug in the next board to flash again,
echo   or close this window to stop.
echo ============================================
echo.
pause

goto FLASH_LOOP