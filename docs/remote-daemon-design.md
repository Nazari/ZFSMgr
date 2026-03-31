---
id: remote-daemon-design
title: Diseño del daemon remoto de ZFSMgr
---

## Objetivo

Desacoplar las consultas y acciones de ZFSMgr de la salida stdout de los binarios `zfs`/`zpool`, sustituyéndolas por un servicio remoto ligero que exponga `libzfs_core` a través de una conexión cifrada y autenticada. El cliente deberá poder reutilizar la interfaz RPC desde cualquier plataforma, con un único puerto dedicado distinto de SSH (p.ej. 32099) y con autenticación basada en clave pública generada por la aplicación.

## Componentes principales

1. **Daemon remoto**
   - Servidores binarios distintos para Linux, FreeBSD, macOS y Windows (en este último caso se emula el backend con `zfs.exe` y PowerShell porque `libzfs_core` no está disponible).
   - Usa `libssh` embebido para exponer una API RPC JSON/CBOR.
   - Cada sesión cliente está autenticada con la clave pública de ZFSMgr; el daemon mantiene una whitelist y solo acepta la firma válida.
   - El daemon escucha en puerto `32099`, configurable pero reservado para esta funcionalidad (sin conflicto con servicios oficiales), y rechaza conexiones desde fuera de la whitelist.

2. **Cliente ZFSMgr**
   - Detecta presencia del daemon antes de ejecutar comandos remotos; si responde, lo utiliza para la consulta/acción.
   - Ofrece fallback al flujo actual vía SSH para operaciones no cubiertas por la API o cuando el daemon no responde.
   - Genera/rota las claves públicas/privadas y las replica en los hosts remotos autorizados.

3. **Claves ZFSMgr**
   - Se crea un par Ed25519 en la máquina cliente; la privada se guarda cifrada y la pública se copia a los daemons.
   - La clase de conexión `DaemonTransport` firma cada petición y el daemon valida la firma antes de ejecutar cualquier comando.

## API y operaciones

El daemon ofrece llamadas RPC similares al stack actual:

- `GET /pools` – lista pools (`PoolInfo`) a partir de `libzfs_core`.
- `GET /datasets?pool=<pool>` – lista datasets con `DSInfo` y `runtime`.
- `GET /datasets/{dataset}/properties` – devuelve propiedades detalladas, `GSA`, `mountpoint`, etc.
- `POST /datasets/{dataset}/action` – ejecuta acciones (`snapshot`, `mount`, `destroy`, `set`, `send/recv` pero delega operaciones masivas a SSH). La acción se identifica por `type`, `recursive` y parámetros adicionales.
- `GET /gsa/schedule` – replica el estado del GSA actual del pool.
- `GET /health` – estado operativo del daemon (permite fallback automático si el daemon está reiniciando).

Las respuestas devuelven estructuras idénticas (`PoolInfo`, `DSInfo`, `PendingAction`, `GsaPlan`) para mantener compatibilidad con el modelo interno de ZFSMgr.

### RPC core

El daemon implementa un servidor RPC ligero sobre el canal `libssh`. Cada conexión establece un canal cifrado, envía paquetes JSON con los campos `id`, `method` y `params`, y espera una respuesta `result`/`error`. El servidor mantiene un `HandlerRegistry` que corresponde cada ruta (por ejemplo `/datasets/{name}/properties`) con una función que invoca `libzfs_core` (o proxies Windows) y serializa el resultado.

Las rutas iniciales son:

- `GET /pools`: lista `PoolInfo` educida desde `libzfs_core_get_pool_list()`.
- `GET /pools/{pool}/datasets`: devuelve la lista de `DSInfo` para un pool.
- `GET /datasets/{dataset}/properties`: propiedades completas de dataset/snapshot (`zfs get -H -o property,value`).
- `POST /datasets/{dataset}/actions`: ejecuta el action dispatcher (mount, set, snapshot, destroy, send/recv). Las acciones masivas (`send/recv`, `rsync`, `tar`) devuelven el campo `fallback: "ssh"` para que ZFSMgr lance el comando tradicional.
- `GET /gsa/{pool}`: estado de GSA, plan de snapshots y reglas heredadas.
- `GET /health`: estado del daemon (`uptime`, colas, última consulta) que el cliente usa para decidir reconexiones.

Cada handler retorna código `200`/`400`/`500`, etiquetas `error.code` y un `audit_id` para correlacionar con los logs.

### Autenticación

Usamos la clave Ed25519 generada en `scripts/generate-daemon-keys.sh`:

1. ZFSMgr firma cada petición con su clave privada, el daemon valida la firma antes de ejecutar la ruta.
2. La whitelist del daemon consiste en el fingerprint publicado en `/etc/zfsmgr/fingerprint` (Windows: `%PROGRAMDATA%\ZFSMgr\fingerprint`).
3. La rotación lógicamente se hace con `zfsmgr key rotate`: el cliente distribuye la nueva pública, actualiza la whitelist y mantiene la clave antigua hasta una fecha de expiración.

El daemon rechaza cualquier sesión no verificada (HTTP-like 401) y graba en log la IP + fingerprint.

### Backend `libzfs_core` y Windows proxy

En Linux/FreeBSD/macOS el daemon enlaza con `libzfs_core` y expone funciones equivalentes a las que usa el cliente:

- `PoolInfo getPoolInfo(libzfs_handle_t*)`
- `DSInfo getDatasetInfo(pool, dataset)`
- `QStringList getDatasetProperties(dataset, snapshot?)`
- `ActionResult runAction(dataset, action)`
- `GsaPlan getGsaPlan(pool)`

El daemon mantiene un `LockManager` y serializa accesos a un pool/dataset para evitar concurrencias destructivas.

En Windows, una capa `WindowsZfsBackend` ejecuta `zfs.exe` y usa PowerShell para recabar la misma salida formateada en JSON; el handler mapea la respuesta a `PoolInfo`/`DSInfo`.

### Concurrencia y salud

- Cola de peticiones (`std::deque<Request>` con `std::mutex`) y pool de workers.
- `GET /health` refleja `status`, `uptime`, `queue_length`, `last_error` y `current_action`.
- Logs con trazas `RPC [method] -> status 200/500/601` y métricas exported a JSON para monitoreo.

### Documentación y ayuda

Se debe ampliar la ayuda/README con cómo:

1. Generar claves (`scripts/generate-daemon-keys.sh`).
2. Desplegar el daemon (`scripts/deploy-daemon.{sh,ps1}`).
3. Consumir la API RPC y tratar fallbacks SSH (`rsync`, `tar`).
4. Monitorear `/health` y rotar fingerprints.

El nuevo flujo se ilustra con un diagrama cliente → daemon RPC → `libzfs_core` → UI.

## Gestión de la autenticación

1. ZFSMgr genera el par de claves (`zfsmgr_id_ed25519` / `zfsmgr_id_ed25519.pub`) y almacena la privada cifrada con el `keyring` local.
2. El instalador remoto coloca la pública en `/etc/zfsmgr/allowed_keys` (u equivalente en Windows).
3. El daemon valida cada petición al nivel de `libssh` antes de ejecutar el RPC.
4. Rotación: basta con actualizar la key pública y reiniciar el daemon; el cliente acepta la nueva clave al tiempo que mantiene la antigua hasta una fecha de expiración.

## Instalación remota y apertura de puertos

Scripts `deploy-daemon.{sh,ps1}` realizan los pasos:

1. Copiar binarios a `/opt/zfsmgr-daemon/` y habilitar `systemd`/`launchd`/servicio NSSM.
2. Plasmar la key pública en `/etc/zfsmgr/allowed_keys`. 3. Habilitar `32099`:
   - Linux: `ufw allow 32099/tcp` o `firewall-cmd --add-port=32099/tcp --permanent` + reload.
   - FreeBSD: `pfctl -f /etc/pf.conf` con regla que permita 32099.
   - macOS: `pfctl` similar y `socketfilterfw --add` si procede.
   - Windows: `netsh advfirewall firewall add rule name="ZFSMgr daemon" dir=in action=allow protocol=TCP localport=32099`.
4. Registrar el daemon con un servicio para arrancar al inicio y exponer endpoints `/health` y `/version`.
5. El script anota los pasos en un log y devuelve un JSON con el endpoint y el fingerprint activo.

## Operaciones que siguen usando SSH

- `rsync`, `tar` y cualquier transferencia raw siguen ejecutándose por SSH directo, ya que requieren canal seguro de alto rendimiento y control sobre los datos.
- El daemon solo expone la interfaz de control: listado de pools/datasets, propiedades, gestión de permisos y GSA.

## Fallback y detección

1. ZFSMgr intenta conectarse al daemon via `libssh://<host>:32099`.
2. Si no responde, cae a SSH tradicional y marca el host como “sin daemon”.
3. Los logs conservan información del modo usado para cada acción para facilitar auditoría.

## Documentación adicional

Se debe añadir una sección en la ayuda y en el README que describa:

- Cómo instalar el daemon (`deploy-daemon.sh`/`deploy-daemon.ps1`).
- Cómo generar/rotar las claves (`zfsmgr keygen`).
- Qué acciones pasan al daemon y cuáles siguen por SSH.
- Cómo monitorizar el puerto 32099 y la salud del servicio (`GET /health`).

Un esquema complementario describe los pasos: cliente detecta daemon → RPC JSON → `libzfs_core` → respuesta → UI.
