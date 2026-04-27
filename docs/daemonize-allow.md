# Daemonización de zfs allow batch

## Problema

La lectura batch de permisos ZFS (`zfs allow`) para múltiples datasets se ejecutaba siempre por SSH, lanzando un script de shell con un bucle `for`:

```sh
set +e
for ds in 'pool/ds1' 'pool/ds2'; do
  printf '__ZFSMGR_ALLOW_BEGIN__ %s\n' "$ds"
  zfs allow "$ds" 2>&1
  printf '__ZFSMGR_ALLOW_RC__ %s %s\n' "$ds" "$?"
  printf '__ZFSMGR_ALLOW_END__ %s\n' "$ds"
done
```

El script usa delimitadores `__ZFSMGR_ALLOW_BEGIN__` / `__ZFSMGR_ALLOW_RC__` / `__ZFSMGR_ALLOW_END__` para separar la salida de cada dataset. La GUI parsea esos marcadores en `mainwindow_permissions.cpp` (~línea 876).

## Solución

Nuevo handler en el daemon: `--dump-zfs-allow-batch <ds1> [ds2 ...]`

El daemon recibe la lista de datasets como params separados, itera sobre ellos ejecutando `zfs allow` para cada uno, y produce **exactamente el mismo formato de delimitadores** que el script de shell. El código de parseo de la GUI no cambia.

## Handler del daemon (`src/daemon_main.cpp`)

Insertado después de `--dump-zfs-allow`, en `executeAgentCommandCapture()`:

```cpp
if (cmd == "--dump-zfs-allow-batch") {
    if (params.empty()) {
        r.rc = 2;
        r.err = std::string("usage: ") + argv0 + " --dump-zfs-allow-batch <ds1> [ds2...]\n";
        return r;
    }
    r.rc = 0;
    for (const auto& ds : params) {
        r.out += "__ZFSMGR_ALLOW_BEGIN__ " + ds + "\n";
        ExecResult res = runExecCapture("zfs", {"allow", ds});
        r.out += res.out;
        if (!res.err.empty()) r.out += res.err;   // equivale al 2>&1 del script
        r.out += "__ZFSMGR_ALLOW_RC__ " + ds + " " + std::to_string(res.rc) + "\n";
        r.out += "__ZFSMGR_ALLOW_END__ " + ds + "\n";
        if (res.rc != 0) r.rc = res.rc;
    }
    return r;
}
```

## Cambio en la GUI (`src/mainwindow_permissions.cpp`)

En la función que construye `batchCommand` (~línea 858), se añade la condición `daemonReadApiOk` idéntica a la usada en `zfs diff` y `zfs allow` individual:

```cpp
const bool daemonReadApiOk = !isWindowsConnection(p)
    && connIdx >= 0 && connIdx < static_cast<int>(m_states.size())
    && m_states[connIdx].daemonInstalled
    && m_states[connIdx].daemonActive
    && m_states[connIdx].daemonNativeBinary
    && m_states[connIdx].daemonApiVersion.trimmed()
           == agentversion::expectedApiVersion().trimmed();

const QString batchCommand = daemonReadApiOk
    ? QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-allow-batch %1")
          .arg(quoted.join(QLatin1Char(' ')))
    : withSudo(p, mwhelpers::withUnixSearchPathCommand(batchScript));
```

El `fetchConnectionCommandOutput` se llama igual; solo cambia el valor de `batchCommand`.

## Compatibilidad hacia atrás

- Si `daemonReadApiOk == false`, se usa el script de shell SSH clásico — sin cambio de comportamiento.
- No hay bump de `kApiVersion`; el handler es aditivo.
- Datasets con nombres que contienen espacios: la GUI ya los pasa con `shSingleQuote()` antes de unirlos, por lo que el daemon los recibe correctamente como params separados a través del parser de args del agente.

## Verificación

```sh
# En el host remoto con daemon activo:
zfsmgr-agent --dump-zfs-allow-batch pool/ds1 pool/ds2

# Salida esperada:
__ZFSMGR_ALLOW_BEGIN__ pool/ds1
---- Permissions on pool/ds1 ----
...
__ZFSMGR_ALLOW_RC__ pool/ds1 0
__ZFSMGR_ALLOW_END__ pool/ds1
__ZFSMGR_ALLOW_BEGIN__ pool/ds2
...
```

En GUI: abrir permisos de un pool con varios datasets → log no muestra SSH sudo, muestra RPC.
