@echo off
title Skate3Recomp custom-music build
echo Building Skate3Recomp with custom music. This window must stay open.
echo Approve any Windows "User Account Control" prompts for tool installs.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
echo.
pause
