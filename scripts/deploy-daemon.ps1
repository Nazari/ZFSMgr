param(
    [Parameter(Mandatory=$true)]
    [string]$Host,
    [Parameter(Mandatory=$true)]
    [string]$BinaryPath
)

$Destination = 'C:\Program Files\ZFSMgr\daemon'
$Port = 32099
Write-Host "Copiando binario y configurando servicio en $Host"
Invoke-Command -ComputerName $Host -ScriptBlock {
    param($dst, $bin)
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Copy-Item -Path $bin -Destination (Join-Path $dst 'zfsmgr_daemon.exe') -Force
    $svc = Get-Service -Name 'ZFSMgrDaemon' -ErrorAction SilentlyContinue
    if ($svc) {
        Stop-Service -Name 'ZFSMgrDaemon' -Force
    }
    New-Service -Name 'ZFSMgrDaemon' -BinaryPathName "$dst\zfsmgr_daemon.exe --port $Port" -DisplayName 'ZFSMgr daemon' -StartupType Automatic
    Start-Service -Name 'ZFSMgrDaemon'
    netsh advfirewall firewall add rule name="ZFSMgr daemon" dir=in action=allow protocol=TCP localport=$Port | Out-Null
} -ArgumentList $Destination, $BinaryPath
