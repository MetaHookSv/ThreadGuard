@echo off
setlocal
set "Configuration=Release"
call "%~dp0build-ThreadGuard-x86.bat" %*
exit /b %errorlevel%
