# Daemonización de comandos ZFS/ZPool

## Contexto

ZFSMgr tiene dos mecanismos para ejecutar comandos en el host remoto:

- **SSH directo**: Se abre una sesión SSH y se ejecuta el comando en el shell remoto.
- **SSH → daemon CLI** (`daemonReadApiOk`): Se ejecuta `/usr/local/libexec/zfsmgr-agent --dump-*` vía SSH cuando el daemon está instalado, activo, es un binario nativo y su versión de API coincide. El daemon corre como root, por lo que no requiere `sudo` ni variaciones de PATH.

Hay además un tercer mecanismo de **RPC real** (túnel mTLS sobre SSH) usado exclusivamente para operaciones de transferencia async (`--zfs-send-to-peer-async`, `--zfs-recv-listen`). Ese mecanismo no aplica a lecturas simples.

## Patrón `--dump-*`

Los handlers de lectura en el daemon siguen el patrón:

```cpp
if (cmd == "--dump-zfs-foo") {
    if (params.size() < N) {
        r.rc = 2;
        r.err = std::string("usage: ") + argv0 + " --dump-zfs-foo <arg1> ...\n";
        return r;
    }
    return runExecCapture("zfs", {"foo", params[0], ...});
}
```

Se añaden en `executeAgentCommandCapture()` en `src/daemon_main.cpp`. `runExecCapture` captura stdout, stderr y rc del proceso hijo.

La GUI selecciona la ruta con:

```cpp
const bool daemonReadApiOk = !isWindowsConnection(p)
    && connIdx >= 0 && connIdx < static_cast<int>(m_states.size())
    && m_states[connIdx].daemonInstalled
    && m_states[connIdx].daemonActive
    && m_states[connIdx].daemonNativeBinary
    && m_states[connIdx].daemonApiVersion.trimmed()
           == agentversion::expectedApiVersion().trimmed();

const QString cmd = daemonReadApiOk
    ? QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-foo %1").arg(arg)
    : withSudo(p, mwhelpers::withUnixSearchPathCommand(
          QStringLiteral("zfs foo %1").arg(arg)));
```

## Inventario: estado de daemonización

### Ya daemonizados (daemon handler + GUI condicional)

| Comando | Handler daemon | Archivo GUI |
|---|---|---|
| `zpool list -j` | `--dump-zpool-list` | `mainwindow_refresh.cpp` |
| `zfs mount -j` | `--dump-zfs-mount` | `mainwindow_refresh.cpp` |
| `zpool import; zpool import -s` | `--dump-zpool-import-probe` | `mainwindow_refresh.cpp` |
| `zpool get guid <pool>` | `--dump-zpool-guid` | `mainwindow_refresh.cpp` |
| `zpool status -v <pool>` | `--dump-zpool-status` | `mainwindow_refresh.cpp` |
| `zpool status -P <pool>` | `--dump-zpool-status-p` | `mainwindow_refresh.cpp` |
| `zpool history <pool>` | `--dump-zpool-history` | `mainwindow_pools.cpp` |
| `zpool get all <pool>` | `--dump-zpool-get-all` | `mainwindow_pools.cpp` |
| `zfs list ... -r` | `--dump-zfs-list-all` | `mainwindow_refresh.cpp` |
| `zfs get -j all <dataset>` | `--dump-zfs-get-all` | `mainwindow_dataset_props.cpp` |
| `zfs get -j <props> <dataset>` | `--dump-zfs-get-json` | varios |
| `zfs get -H -o value <prop> <dataset>` | `--dump-zfs-get-prop` | varios |
| `zfs diff <snap1> <snap2>` | `--dump-zfs-diff` | `mainwindow_transfer.cpp` |
| `zfs allow <dataset>` | `--dump-zfs-allow` | `mainwindow_permissions.cpp` |

### Fuera de alcance (SSH por diseño)

| Comando | Razón |
|---|---|
| `zfs send / zfs recv` | Stream binario; requiere pipe SSH. Transferencias daemon-a-daemon usan `--zfs-send-to-peer[-async]` (ver `docs/daemon2daemon.md`). |
| Scripts compuestos (From Dir / To Dir) | Mezclan tar, rm, zfs mount en un solo script de shell; requerirían un RPC dedicado. |

## Handlers añadidos en este sprint

### `--dump-zfs-diff <snap1> <snap2>`

Ejecuta `zfs diff snap1 snap2`. Salida: líneas tab-separadas con carácter de operación (`+`, `-`, `M`, `R`) y ruta(s). Misma salida que el comando directo; el parseo en `actionDiffSnapshot()` no cambia.

### `--dump-zfs-allow <dataset>`

Ejecuta `zfs allow dataset`. Salida: texto estructurado por secciones (local, descendant, create time, permission sets). Misma salida que el comando directo; el parseo en `parsePermissionsOutput()` no cambia.

## Compatibilidad hacia atrás

- Si `daemonReadApiOk == false` (daemon inactivo, versión distinta, o Windows), la GUI cae al path SSH clásico sin cambio de comportamiento.
- No hay bump de `kApiVersion`. Los handlers son aditivos.
- Un daemon antiguo que no tenga `--dump-zfs-diff` o `--dump-zfs-allow` nunca será seleccionado por la GUI (el check de versión de API lo impide) mientras el daemon se actualice al desplegarse la nueva versión.
