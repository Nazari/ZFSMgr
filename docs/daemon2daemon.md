# Diseño técnico: transferencias daemon-a-daemon en background

## Motivación

Las operaciones Copy/Level bloquean la GUI hasta que completan. El objetivo es que el usuario pueda cerrar la GUI y la transferencia continúe ejecutándose entre los daemons. Al reabrir la GUI, debe poder ver los jobs en curso y cancelarlos.

## Alcance

| Modo | Descripción | ¿Cubierto? |
|------|------------|------------|
| **Mode 1** — daemon-a-daemon | Los datos fluyen directamente entre daemons via TCP; la GUI no está en el datapath | ✅ Objetivo principal |
| **Mode 2** — mismo host (SSH pipe) | `zfs send \| zfs recv` en el mismo host remoto | ✅ Delegado al daemon |
| **Mode 3** — piped por GUI host | Los datos pasan por la máquina con la GUI | ❌ Fuera de alcance — GUI está en el datapath |

---

## Arquitectura del daemon

### Job registry

```cpp
// daemon_main.cpp — zona de globals (junto a g_stop, g_daemonLogMutex)

constexpr const char* kJobsFilePath = "/var/lib/zfsmgr/jobs.json";

enum class JobState { Queued, Running, Done, Failed, Cancelled };

struct DaemonJob {
    std::string id;               // 16-hex UUID (generado con genToken())
    std::string type;             // "send-to-peer" | "pipe-local"
    std::string snap;             // snapshot origen (p.ej. "pool/ds@snap")
    std::string peerHost;         // host destino
    int         peerPort{0};      // puerto TCP del receptor
    std::string token;            // token de autenticación 64-hex
    std::string baseSnap;         // snapshot base para incremental (vacío = full)
    std::string sendFlags;        // flags de zfs send: "-R", "-c", etc.
    std::string pipeCmd;          // cmd base64 para mode 2 (pipe-local)

    JobState    state{JobState::Queued};
    pid_t       sendPid{-1};      // PID del proceso zfs send (para SIGTERM)
    uint64_t    bytesTransferred{0};
    double      rateMiBs{0.0};
    long        elapsedSecs{0};
    std::string startedAtUtc;     // ISO-8601
    std::string finishedAtUtc;    // ISO-8601, vacío mientras running
    std::string errorText;
    std::vector<std::string> progressLines; // ring buffer, últimas 5 líneas BYTES=
};

std::mutex g_jobsMutex;
std::unordered_map<std::string, DaemonJob> g_jobs;  // id → job
```

### Persistencia

**`persistJobsLocked()`** — llamado bajo `g_jobsMutex`. Serializa `g_jobs` a `/var/lib/zfsmgr/jobs.json` como array JSON usando el helper existente `jsonEscape()`. Mantiene un máximo de 20 jobs; el pruning elimina los más antiguos entre Done/Failed/Cancelled (los Running y Queued no se eliminan nunca).

**`loadPersistedJobsAtStartup()`** — llamado una vez al inicio de `runServeLoop()`, antes del accept loop. Deserializa el JSON. Jobs con `state=Running` transicionan a `Failed` con `errorText = "daemon restarted while running"`. Esto da semántica de reinicio limpia.

### Ejecución async del send (Mode 1)

**`runZfsSendToPeerAsync(const std::string& jobId)`** — función estática nueva en `daemon_main.cpp`, inmediatamente después de `runZfsSendToPeerCapture`.

Comportamiento:
1. Lee los parámetros del job bajo `g_jobsMutex`.
2. Ejecuta el relay loop existente de `runZfsSendToPeerCapture` (movido sin cambio estructural).
3. Almacena `sendPid` justo después del `fork()`, bajo mutex.
4. Cada 2 segundos dentro del relay: actualiza `bytesTransferred`, `rateMiBs`, `elapsedSecs`, `progressLines` bajo mutex.
5. Al inicio de cada iteración del relay: comprueba `state == Cancelled` para salida limpia si fue cancelado.
6. Al terminar: `state = Done` (o `Failed`), `finishedAtUtc = utcNowIsoString()`, llama `persistJobsLocked()`.

**`runZfsPipeLocalAsync(const std::string& jobId)`** — variante para Mode 2. Decodifica `pipeCmd` de base64, lanza `sh -c <cmd>` como proceso hijo, almacena PID, lee stdout/stderr al ring buffer.

---

## RPCs nuevos del daemon

Todos se añaden inline en el loop RPC de `runServeLoop` con el mismo patrón que `--wait-for-event` (línea ~3100): bypasean el cache y el sistema de invalidación.

### `--zfs-send-to-peer-async`

**Args:** `snap peerHost peerPort token baseSnap sendFlags`
(idéntico a `--zfs-send-to-peer`)

**Respuesta (rc=0):**
```
JOB_ID=a3f7b2c1d4e80912
STATE=running
```

**Comportamiento:** Valida los argumentos, crea `DaemonJob`, lo inserta en `g_jobs`, lanza `std::thread([jobId]{ runZfsSendToPeerAsync(jobId); }).detach()`, llama `persistJobsLocked()`, y retorna inmediatamente.

El RPC `--zfs-send-to-peer` síncrono **no se elimina** — sigue disponible para backward compatibility y scripting directo.

---

### `--job-status`

**Args:** `jobId`

**Respuesta (rc=0):**
```
JOB_ID=a3f7b2c1d4e80912
STATE=running
BYTES=1234567890
RATE_MIB_S=58.4
ELAPSED_SECS=21
STARTED_AT=2026-04-26T14:00:00Z
FINISHED_AT=
ERROR=
PROGRESS_LINE=BYTES=1234567890  1177.6 MiB  @ 58.4 MiB/s  elapsed 21s
```

`STATE` ∈ `queued` | `running` | `done` | `failed` | `cancelled`

**Respuesta (rc=1):**
```
ERROR=job not found
```

---

### `--job-list`

**Args:** _(ninguno)_

**Respuesta (rc=0):** Una línea `JOB=` por job, newest-first, max 20:
```
JOB={"id":"a3f7b2","state":"running","type":"send-to-peer","snap":"pool/ds@snap","bytes":1234567890,"rate":58.4,"elapsed":21,"started":"2026-04-26T14:00:00Z"}
JOB={"id":"b8c01f","state":"done","type":"send-to-peer","snap":"pool/ds@prev","bytes":9876543210,"rate":120.1,"elapsed":82,"started":"2026-04-26T12:00:00Z"}
JOBS_COUNT=2
```

---

### `--job-cancel`

**Args:** `jobId`

**Respuesta (rc=0):**
```
JOB_ID=a3f7b2c1d4e80912
CANCELLED=1
```

**Comportamiento:** Bajo `g_jobsMutex`, si `state == Running`: `kill(sendPid, SIGTERM)`, `state = Cancelled`, `finishedAtUtc = now`, `persistJobsLocked()`. El relay loop en `runZfsSendToPeerAsync` detecta el broken pipe o el flag `Cancelled` y termina limpiamente.

---

### `--zfs-pipe-local-async` (Mode 2) — ELIMINADO

Este RPC aceptaba un comando de shell arbitrario (`b64Cmd`) y lo ejecutaba como
`sh -c <cmd>` con los privilegios del daemon (root). Nunca tuvo un llamador desde
la GUI (Mode 2 no llegó a cablearse), por lo que era superficie de ataque muerta:
cualquier cliente con certificado mTLS obtenía ejecución de shell root arbitraria,
anulando las whitelists tipadas de `--mutate-*`.

Se retiró del daemon (handler, `runZfsPipeLocalAsync()` y su declaración).

**Sustituido por `--zfs-pipe-local` (tipado, síncrono).**

---

### `--mutate-rsync-local` (sync tipado)

**Args:** `payload-b64`

`payload` es base64(JSON array) de strings:

```
[delete("0"|"1"), dryRun("0"|"1"), rsh, dstHost, src1, dst1, src2, dst2, ...]
```

Debe haber al menos un par `src`/`dst` y la lista de rutas debe ser par. Varios
pares cubren el caso de subdatasets, que antes encadenaba N invocaciones de rsync
con `set -e; … && …`: el probe se ejecuta **una vez** y los pares corren en orden,
parando al primer fallo (misma semántica).

`rsh`/`dstHost` van vacíos en sync de mismo host. Con `dstHost`, el destino pasa a
ser `<dstHost>:<dst>/` y `rsh` (si no está vacío) se pasa como `-e`; rsync aplica su
propia tokenización *quote-aware* a esa cadena, por lo que el comportamiento es
idéntico al del shell anterior (verificado empíricamente con rsync 3.4.1).

El daemon ejecuta el sondeo de capacidades que antes iba embebido en el shell
(`rsync --help | grep -- '--info'`, `rsync -A/-X --version`, `--extended-attributes`)
y monta el argv él mismo, ejecutando `rsync` con execvp. **No interviene ningún
shell**, así que la GUI solo aporta rutas y booleanos.

Validación rechazada con rc=2: base64/JSON inválido, número de campos < 6 o lista de
rutas impar, flags que no sean `"0"`/`"1"`, o rutas no absolutas.

---

### `--zfs-pipe-local` (Mode 2, tipado)

**Args:** `payload-b64`

`payload` es base64(JSON array) de **exactamente dos** elementos; cada elemento es
base64(JSON array) con un argv sin el `zfs` inicial:

- elemento 0 debe empezar por `send`
- elemento 1 debe empezar por `recv`

Ejemplo (decodificado): `[["send","-wLecR","tank/ds@snap"], ["recv","-Fus","tank/backup/ds"]]`

El daemon monta la tubería él mismo (`pipe()` + `fork()` ×2 + `execvp("zfs", …)`),
captura el stderr de ambos extremos y devuelve el rc de `recv` (o el de `send` si
`recv` fue 0). **No interviene ningún shell**, así que no se puede expresar nada que
no sea `zfs send … | zfs recv …`.

Validación rechazada con rc=2: número de elementos ≠ 2, base64/JSON inválido, o un
argv cuyo primer token no sea `send`/`recv` respectivamente.

Nota: a diferencia del pipeline de shell anterior, no se interpone `pv`, por lo que
esta ruta síncrona no emite líneas de progreso. El progreso sigue disponible en la
ruta preferente (jobs asíncronos, pestaña *Transferencias*).

---

### Feature flag en `--health`

Se añade `JOBS_SUPPORT=1` al response de `--health` en:
- El path in-server (~línea 3083 de `runServeLoop`)
- El path CLI standalone (~línea 3350 de `main`)

La **ausencia** de esta línea equivale a `false` (backward compat con daemons antiguos).

---

## Arquitectura de la GUI

### Detección de capacidad

**`ConnectionRuntimeState` (`mainwindow.h`):**
```cpp
bool daemonJobsSupported{false};
```

**`mainwindow_refresh.cpp`** — al parsear el health response:
```cpp
state.daemonJobsSupported =
    (hkv.value(QStringLiteral("JOBS_SUPPORT")).trimmed() == QStringLiteral("1"));
```

**Helper en `mainwindow_transfer.cpp`:**
```cpp
const auto isDaemonJobsSupported = [&](int idx) {
    return isDaemonReady(idx)
        && idx >= 0 && idx < m_states.size()
        && m_states[idx].daemonJobsSupported;
};
```

### Struct de tracking en GUI

```cpp
// mainwindow.h
struct ActiveDaemonJob {
    int      srcConnIdx{-1};
    int      dstConnIdx{-1};
    QString  jobId;
    QString  displayLabel;      // "Copiar pool/ds@snap → pool/backup"
    QString  state;             // "running"|"done"|"failed"|"cancelled"
    uint64_t bytesTransferred{0};
    double   rateMiBs{0.0};
    long     elapsedSecs{0};
    QString  lastProgressLine;
    QString  startedAt;
    QString  errorText;
};

QList<ActiveDaemonJob> m_activeDaemonJobs;
QTimer*      m_jobPollTimer{nullptr};   // 2500 ms cadence
QListWidget* m_jobsListWidget{nullptr};
```

### Lanzamiento async — `launchDaemonJobTransfer()`

Nuevo método en `MainWindow`. Llamado desde `actionCopySnapshot` y `actionLevelSnapshot` cuando ambos daemons tienen `JOBS_SUPPORT=1`:

1. `--zfs-recv-listen` → daemon destino (síncrono, rápido) → PORT + TOKEN
2. `--zfs-send-to-peer-async` → daemon origen → JOB_ID
3. Crea `ActiveDaemonJob`, añade a `m_activeDaemonJobs`
4. Abre tab "Transferencias", arranca `m_jobPollTimer`
5. Retorna `true` — **no** llama a `queuePendingShellAction`

### Polling de progreso — `pollDaemonJobs()`

Cadencia 2500 ms. Para cada job Running:
- `--job-status <jobId>` al daemon origen via `tryRunRemoteAgentRpcViaTunnel`
- Parsea key=value → actualiza `ActiveDaemonJob` + `QListWidgetItem`
- Si pasa a Done/Failed: `appLog()` con resultado + trigger refresh del árbol
- Para el timer cuando todos los jobs son terminales

### Reconexión — `scanOrphanedJobsForConnection(int connIdx)`

Llamado cuando una conexión pasa a activa con `daemonJobsSupported`:
- `--job-list` → parsea líneas `JOB=`
- Jobs con `state=running` no presentes en `m_activeDaemonJobs` → añadir
- Arranca `m_jobPollTimer` si no está activo

### Tab "Transferencias" (`mainwindow_ui.cpp`)

Insertado después del tab "Combined log":

```
┌─────────────────────────────────────────────────────────┐
│ [running]  pool/ds@snap → 10.0.0.2   1.2 GiB @ 58 MiB/s  21s │
│ [done]     pool/ds@prev → 10.0.0.3   9.4 GiB @ 120 MiB/s 82s │
│                                                         │
│ [ Refrescar ]  [ Cancelar seleccionado ]                │
└─────────────────────────────────────────────────────────┘
```

### Diálogo al cerrar la GUI

Si hay jobs Running cuando el usuario cierra:

```
N transferencia(s) siguen ejecutándose en el daemon.
¿Cerrar de todas formas? Los jobs continuarán en background.
[ Sí ]  [ No ]
```

---

## Backward compatibility

| Daemon | `isDaemonReady` | `daemonJobsSupported` | Path de transferencia |
|--------|----------------|----------------------|----------------------|
| Antiguo (sin `JOBS_SUPPORT`) | true | false | `queuePendingShellAction` síncrono — sin cambio |
| Nuevo (`JOBS_SUPPORT=1`) | true | true | `launchDaemonJobTransfer` async |

No se cambia `kApiVersion`. El RPC síncrono `--zfs-send-to-peer` no se elimina.

---

## Orden de implementación

### Fase 1 — Daemon
1. `DaemonJob` struct + `g_jobs` + `g_jobsMutex` + `kJobsFilePath`
2. `persistJobsLocked()` + `loadPersistedJobsAtStartup()`
3. `runZfsSendToPeerAsync(jobId)` + handler `--zfs-send-to-peer-async`
4. Handlers `--job-status`, `--job-list`, `--job-cancel`
5. `JOBS_SUPPORT=1` en `--health`
6. `runZfsPipeLocalAsync(jobId)` + handler `--zfs-pipe-local-async`

### Fase 2 — GUI: detección
7. `daemonJobsSupported` en `ConnectionRuntimeState` + parseo en `mainwindow_refresh.cpp`

### Fase 3 — GUI: lanzamiento
8. `ActiveDaemonJob` struct + miembros en `mainwindow.h`
9. `launchDaemonJobTransfer()` + ramificación en Copy y Level
10. `pollDaemonJobs()` + `m_jobPollTimer`

### Fase 4 — GUI: UX
11. Tab "Transferencias" + `QListWidget` + botones
12. `closeEvent` — diálogo de confirmación
13. `scanOrphanedJobsForConnection()` + llamada desde refresh result

---

## Verificación

```bash
# 1. Deploy daemon nuevo a unib.local
systemctl stop zfsmgr-agent && scp ... && systemctl start zfsmgr-agent

# 2. Comprobar feature flag
zfsmgr-agent --health | grep JOBS_SUPPORT
# → JOBS_SUPPORT=1

# 3. Test de job end-to-end (Mode 1)
# En destino:
zfsmgr-agent --zfs-recv-listen pool/backup
# → PORT=52001 TOKEN=abcd1234...

# En origen:
zfsmgr-agent --zfs-send-to-peer-async pool/ds@snap 192.168.1.2 52001 abcd1234 "" "-R"
# → JOB_ID=a3f7b2c1d4e80912  STATE=running

# 4. Polling
watch -n2 'zfsmgr-agent --job-status a3f7b2c1d4e80912'

# 5. Cancelación
zfsmgr-agent --job-cancel a3f7b2c1d4e80912
# → CANCELLED=1

# 6. Persistencia
systemctl restart zfsmgr-agent
zfsmgr-agent --job-list
# → job anterior en state=failed con "daemon restarted while running"

# 7. GUI: cerrar durante transferencia → reabrir → tab Transferencias muestra job activo
# 8. GUI + daemon antiguo: Copy/Level funciona como antes (sin JOBS_SUPPORT)
```

---

### `--repair-alt-mountpoints [apply]`

Repara datasets que quedaron con el mountpoint relocalizado por un sync tar
interrumpido de forma irrecuperable (SIGKILL, corte de luz, sustitución del daemon
a mitad de transferencia). Esos casos no los cubre ni el `trap` del shell antiguo ni
el guard RAII del agente, pero el dataset conserva la propiedad
`org.fc16.zfsmgr:savedmountpoint`, que es la miga de pan necesaria para deshacerlo.

Localiza los datasets con esa propiedad puesta explícitamente (`zfs get -s local`) y,
para cada uno, informa del mountpoint guardado y del actual.

- **Sin argumentos: solo informa.** Es seguro llamarlo para diagnóstico.
- **Con `apply`:** desmonta, restaura `mountpoint` al valor guardado y limpia la
  propiedad, en ese orden.

Si la restauración falla, **la propiedad NO se borra**, de modo que el dataset sigue
siendo detectable en la siguiente pasada. Devuelve rc=1 si hubo algún fallo.

Salida: una línea `STRANDED=` por dataset encontrado, `REPAIRED=` por cada uno
restaurado, y los contadores `STRANDED_COUNT` / `REPAIRED_COUNT` / `FAILED_COUNT`.

Se sirve por RPC además de CLI: no acepta entrada libre (solo el interruptor `apply`)
y actúa exclusivamente sobre datasets que llevan nuestra propia propiedad.
