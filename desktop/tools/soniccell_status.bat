@echo off
rem Progress of the SonicCell sampling per priority group, with the estimated remaining time.
setlocal
set ROOT=%~dp0..
set EXE=%ROOT%\build\hsynth_sampler.exe
set INS=%ROOT%\..\testdata\soniccell
set OUT=%ROOT%\data\library\user\soniccell
set ARGS=--ins "%INS%\SonicCell_Patch.ins" --ins "%INS%\SonicCell_Rhythm.ins" --order "%INS%\order.txt" --out "%OUT%"
"%EXE%" status %ARGS%
echo.
pause
endlocal
