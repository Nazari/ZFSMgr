@echo off
rem Instala ZFSMgr @VERSION@ en Windows. Lanza instalar-windows.ps1, que es quien hace el trabajo.
rem
rem -ExecutionPolicy Bypass SOLO para este proceso: la politica por defecto de Windows no deja
rem ejecutar guiones descargados, y cambiarla para toda la maquina no es cosa de un instalador.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0instalar-windows.ps1" %*
exit /b %ERRORLEVEL%
