@echo off
setlocal

:: Check if both input and output arguments are provided
if "%~2"=="" (
    echo Usage: %~nx0 ^<in image file^> ^<out image file^>
    exit /b 1
)

set "IN_FILE=%~1"
set "OUT_FILE=%~2"

:: Define temporary PFM file paths in the system TEMP directory
set "TEMP_IN=%TEMP%\oidn_in_%RANDOM%.pfm"
set "TEMP_OUT=%TEMP%\oidn_out_%RANDOM%.pfm"

echo [1/4] Converting input to PFM format (gbrpf32le)...
ffmpeg -hide_banner -loglevel warning -y -i "%IN_FILE%" -pix_fmt gbrpf32le "%TEMP_IN%"
if %ERRORLEVEL% NEQ 0 (
    echo Error: FFmpeg failed to convert the input image.
    exit /b %ERRORLEVEL%
)

echo [2/4] Denoising with OIDN...
oidnDenoise --hdr "%TEMP_IN%" -o "%TEMP_OUT%"
if %ERRORLEVEL% NEQ 0 (
    echo Error: oidnDenoise failed.
    del "%TEMP_IN%" 2>nul
    exit /b %ERRORLEVEL%
)

echo [3/4] Converting denoised PFM to output format...
ffmpeg -hide_banner -loglevel warning -y -i "%TEMP_OUT%" "%OUT_FILE%"
if %ERRORLEVEL% NEQ 0 (
    echo Error: FFmpeg failed to encode the output image.
    :: Cleanup partial files before exiting
    del "%TEMP_IN%" 2>nul
    del "%TEMP_OUT%" 2>nul
    exit /b %ERRORLEVEL%
)

echo [4/4] Cleaning up temporary files...
del "%TEMP_IN%" 2>nul
del "%TEMP_OUT%" 2>nul

echo.
echo Success! Denoised image saved to: "%OUT_FILE%"
endlocal