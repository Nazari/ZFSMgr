@echo off
rem Installs ZFSMgr @VERSION@ on Windows. Runs install-windows.ps1, which does the work.
rem
rem -ExecutionPolicy Bypass ONLY for this process: Windows' default policy does not run
rem downloaded scripts, and changing it for the whole machine is not an installer's business.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-windows.ps1" %*
exit /b %ERRORLEVEL%
