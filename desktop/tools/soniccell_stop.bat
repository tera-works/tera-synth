@echo off
rem Ask the running SonicCell sampling to stop after the current note.
rem The patch in progress is sampled again from its start when you resume.
setlocal
set ROOT=%~dp0..
set EXE=%ROOT%\build\hsynth_sampler.exe
set INS=%ROOT%\..\testdata\soniccell
set OUT=%ROOT%\data\library\user\soniccell
set ARGS=--ins "%INS%\SonicCell_Patch.ins" --ins "%INS%\SonicCell_Rhythm.ins" --order "%INS%\order.txt" --out "%OUT%"
if not exist "%OUT%" mkdir "%OUT%"
type nul > "%OUT%\STOP"
echo Stop requested: sampling ends after the current note (a few seconds).
echo Resume any time with soniccell_start.bat.
timeout /t 5 >nul
endlocal
