# Activa OpenSSH Server en ESTA máquina, para que ZFSMgr pueda gestionarla en remoto.
#
# Por qué existe: ZFSMgr habla con una máquina Windows SOLO por SSH —el RPC del agente
# viaja por un túnel sobre esa conexión—, así que sin sshd no hay forma de llegar ni
# siquiera para instalar el agente. Es el huevo y la gallina que obligaba a dar con tres
# comandos de PowerShell y ejecutarlos a mano en cada equipo nuevo.
#
# TRES REGLAS, y las tres salen de que la primera versión de esto colgó una instalación:
#
# 1. NUNCA colgarse. `Add-WindowsCapability -Online` es una operación de servicing: si la
#    característica no está en local se la pide a Windows Update, y sin salida a internet
#    puede tardar muchísimo o no volver. Va con plazo y, si se agota, se sigue.
# 2. NUNCA fallar. Este script corre desde el instalador; que no se pueda activar SSH no
#    puede impedir que ZFSMgr quede instalado. Sale siempre con 0.
# 3. SIEMPRE dejar rastro. Corre oculto, así que sin registro un fallo es invisible. Todo
#    va a %TEMP%\zfsmgr-openssh.log y también junto al ejecutable si se puede.

$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'   # sin esto la barra de DISM ralentiza mucho

$logPaths = @("$env:TEMP\zfsmgr-openssh.log")
if ($PSScriptRoot) { $logPaths += (Join-Path $PSScriptRoot 'zfsmgr-openssh.log') }

function Write-Log([string]$msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
    foreach ($p in $logPaths) {
        try { Add-Content -Path $p -Value $line -ErrorAction SilentlyContinue } catch { }
    }
}

Write-Log "=== Activando OpenSSH Server ==="
Write-Log ("Windows: " + (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue).Caption)

# --- 1. La característica -----------------------------------------------------------
$capName = $null
try {
    $cap = Get-WindowsCapability -Online -Name 'OpenSSH.Server*' -ErrorAction Stop |
           Select-Object -First 1
    if ($cap) {
        $capName = $cap.Name
        Write-Log "Capacidad $($cap.Name): estado $($cap.State)"
    } else {
        Write-Log "No se encontró la capacidad OpenSSH.Server en este Windows"
    }
} catch {
    Write-Log "No se pudo consultar la capacidad: $($_.Exception.Message)"
}

if ($capName -and $cap.State -ne 'Installed') {
    Write-Log "Instalando $capName (plazo: 240 s)"
    # En segundo plano y con plazo: es el paso que puede irse a Windows Update y no
    # volver. Si se agota, se deja correr por su cuenta y se sigue con lo demás; puede
    # terminar sola más tarde.
    $job = Start-Job -ScriptBlock {
        Add-WindowsCapability -Online -Name $using:capName -ErrorAction SilentlyContinue | Out-Null
    }
    if (Wait-Job $job -Timeout 240) {
        Receive-Job $job -ErrorAction SilentlyContinue | Out-Null
        Write-Log "Instalación de la capacidad terminada"
    } else {
        Write-Log "PLAZO AGOTADO instalando la capacidad. Suele ser falta de acceso a"
        Write-Log "Windows Update. Continúo; puede completarse sola más adelante."
    }
    Remove-Job $job -Force -ErrorAction SilentlyContinue
} elseif ($capName) {
    Write-Log "La capacidad ya estaba instalada, no se toca"
}

# --- 2. El servicio -----------------------------------------------------------------
$svc = Get-Service -Name sshd -ErrorAction SilentlyContinue
if ($svc) {
    try {
        Set-Service -Name sshd -StartupType Automatic -ErrorAction Stop
        Write-Log "sshd puesto en arranque automático"
    } catch { Write-Log "No se pudo automatizar sshd: $($_.Exception.Message)" }
    try {
        if ($svc.Status -ne 'Running') { Start-Service sshd -ErrorAction Stop }
        Write-Log "sshd en ejecución"
    } catch { Write-Log "No se pudo arrancar sshd: $($_.Exception.Message)" }
} else {
    Write-Log "El servicio sshd NO existe: la capacidad no llegó a instalarse."
    Write-Log "Ejecute a mano, con internet disponible:"
    Write-Log "  Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0"
}

# --- 3. El cortafuegos --------------------------------------------------------------
# Sin esta regla el servicio arranca y aun así no se llega desde fuera: sshd corriendo y
# la conexión rechazada es el síntoma más desconcertante de los tres.
try {
    if (-not (Get-NetFirewallRule -Name 'sshd' -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -Name 'sshd' -DisplayName 'OpenSSH Server (sshd)' `
            -Enabled True -Direction Inbound -Protocol TCP -Action Allow `
            -LocalPort 22 -ErrorAction Stop | Out-Null
        Write-Log "Regla de cortafuegos creada para el puerto 22"
    } else {
        Write-Log "La regla de cortafuegos ya existía"
    }
} catch { Write-Log "No se pudo crear la regla de cortafuegos: $($_.Exception.Message)" }

$final = Get-Service -Name sshd -ErrorAction SilentlyContinue
Write-Log ("Resultado: sshd " + $(if ($final) { $final.Status } else { 'no instalado' }))
Write-Log "=== Fin ==="

exit 0
