@echo off
title Skate3 custom music: convert tracks
echo Converting your Music folder for Skate 3. Approve a UAC prompt if FFmpeg needs installing.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0prepare_music.ps1"
echo.
pause
