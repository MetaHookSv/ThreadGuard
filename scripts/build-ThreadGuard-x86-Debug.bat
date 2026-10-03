@echo off
setlocal
set "Configuration=Debug"
call "%~dp0build-ThreadGuard-x86.bat" %*
exit /b %errorlevel%
