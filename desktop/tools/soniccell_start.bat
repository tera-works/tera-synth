@echo off
rem Start or resume sampling the Roland SonicCell (important sounds first, see soniccell_order.py).
rem Finished patches are skipped. Stop with soniccell_stop.bat, Ctrl+C or by closing this window.
rem Finished patches also get FLAC copies next to their WAVs, in parallel (flac_library.py --watch).
rem Output is for testing only - do not distribute.
setlocal
title SonicCell sampling
set ROOT=%~dp0..
set EXE=%ROOT%\build\hsynth_sampler.exe
set INS=%ROOT%\..\testdata\soniccell
set OUT=%ROOT%\data\library\user\soniccell
set ARGS=--ins "%INS%\SonicCell_Patch.ins" --ins "%INS%\SonicCell_Rhythm.ins" --order "%INS%\order.txt" --out "%OUT%"
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\STOP" del "%OUT%\STOP"
rem FLAC copies of finished patches, made alongside in a second window (WAVs are kept; ends by itself)
start "SonicCell FLAC" /min python "%~dp0flac_library.py" "%OUT%" --watch
"%EXE%" batch --midi-out SonicCell --capture-device SonicCell --volume 127 --decimate 2 %ARGS% --prefix "SC " --log "%OUT%\capture_log.txt"
echo.
"%EXE%" status %ARGS%
echo.
pause
endlocal
