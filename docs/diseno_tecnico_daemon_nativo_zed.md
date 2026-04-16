# Diseño técnico: sustitución de scripts remotos por daemons nativos

## Objetivo

Sustituir el modelo actual basado en scripts remotos (invocados por SSH) por un daemon nativo por sistema operativo soportado, de modo que:

- el estado ZFS/ZPool requerido por ZFSMgr se mantenga en memoria en el host remoto,
- las actualizaciones del estado se apliquen de forma reactiva por eventos (`zed`) en lugar de sondeo continuo,
- la comunicación ZFSMgr <-> daemon sea autenticada por certificado y cifrada,
- ZFSMgr pueda desplegar/actualizar/iniciar el daemon automáticamente al conectar.

## Sistemas objetivo

- Linux (systemd + zed)
- FreeBSD (rc.d/service + zed)
- macOS (launchd + evento equivalente; sin `zed` oficial en todas las instalaciones)
- Windows (servicio nativo + fuente de cambios alternativa)

Nota: `zed` aplica de forma natural a plataformas OpenZFS tipo Unix. Para macOS/Windows el mecanismo de eventos puede requerir adaptador específico.

## Arquitectura propuesta

### Componentes

1. `zfsmgr-agent` (daemon remoto)
- Proceso residente con caché en memoria.
- API TLS para ZFSMgr.
- Recolector de estado inicial (comandos locales `zfs` / `zpool`).
- Ingestor de eventos (`zed`/zevents) para refresco incremental.
- Reconciliador periódico de baja frecuencia.

2. `zfsmgr-bootstrap` (instalador/actualizador remoto)
- Binario/script mínimo de bootstrap.
- Instala binario agent + unidad de servicio + certificados + configuración.
- Idempotente y versionado.

3. `zfsmgr-control` (en GUI)
- Detecta capacidad daemon por conexión.
- Negocia versión API.
- Si falta daemon o está obsoleto: despliegue seguro automático.
- Cambia transporte de SSH-comandos a RPC del daemon cuando esté disponible.

### Flujo de conexión

1. ZFSMgr conecta por SSH como hoy (canal de bootstrap).
2. Verifica si `zfsmgr-agent` existe y está activo.
3. Si no existe/obsoleto:
- sube paquete firmado,
- instala servicio,
- provisiona certificados,
- arranca daemon,
- valida health check TLS.
4. A partir de ahí, operaciones de lectura/escritura van por API segura del daemon.

## Seguridad

### Canal de control y datos

- TLS 1.3 obligatorio.
- mTLS (certificado cliente ZFSMgr + certificado servidor agent).
- Pinning de CA de ZFSMgr por conexión.
- Rotación de certificados con validez corta.

### Bootstrap

- Se mantiene SSH solo para bootstrap/recuperación.
- Verificación de host key estricta (sin `StrictHostKeyChecking=no`).
- Artefactos firmados (firma del release) y checksum.

### Autorización

- ACL por conexión y por operación (read-only / mutating).
- Auditoría local en daemon (`who`, `what`, `when`, `result`).

## Modelo de datos en memoria

La caché del daemon debe incluir como mínimo:

- pools, topología vdev, estado, health,
- datasets y snapshots,
- propiedades visibles en UI,
- capacidades/comandos detectados,
- metadata GSA y conectividad relevante.

Estructura recomendada:

- índice por `pool_guid`, `dataset_guid`, `snapshot_guid`,
- vistas derivadas para UI (sin recalcular todo en cada consulta),
- versionado monotónico interno (`state_version`) para delta-poll desde GUI.

## Carga inicial (obligatoria por CLI local)

La carga inicial debe ejecutarse con comandos locales `zfs`/`zpool`:

- `zpool list/get/status -P ...`
- `zfs list/get ...`
- `zpool history` y/o `zfs history` opcional para reconstrucción adicional

Objetivo:

- construir estado base coherente antes de aceptar peticiones UI,
- fijar cursor de eventos (`last_eid` o equivalente) tras snapshot inicial.

## Refresco por eventos

### Fuente principal

- `zed` / zevents (`zpool events -f -v` o integración directa)
- consumo con cursor de EID.

### Procesamiento

- traducir evento -> entidades afectadas,
- refresco selectivo por pool/dataset afectado,
- invalidación granular de nodos cacheados.

## Verificación: ¿son suficientes los eventos de zed para mantener TODO el estado?

Conclusión: **no, por sí solos no son suficientes para garantizar coherencia total en todos los casos**.

Motivos técnicos:

1. La documentación de eventos está centrada en salud/pool/vdev y no cubre de forma completa todo cambio lógico de datasets/snapshots/props en una API contractual estable.
2. La propia documentación histórica de eventos indica cobertura incompleta/no plenamente documentada.
3. Los eventos son "recientes" en kernel y pueden perderse por reinicios, recarga de módulo, caída del daemon o limpieza de cola.

Implicación de diseño:

- usar `zed` como **fuente primaria incremental**,
- añadir **reconciliación periódica** (ej. cada 5-15 min o adaptativa),
- ejecutar **rebuild selectivo** al detectar gap de EID/overflow/restart,
- soporte de **full resync bajo demanda** desde UI.

## Estrategia híbrida recomendada (obligatoria)

1. Initial full snapshot de estado (CLI local).
2. Event loop continuo por zevents.
3. Reconciliador periódico de bajo coste:
- comprobar contadores/versiones,
- verificar existencia de datasets/snapshots clave,
- corregir divergencias.
4. Full resync automático cuando:
- `eid` no contiguo,
- arranque tras downtime largo,
- error de parser de evento,
- detección explícita de inconsistencia.

## API propuesta (resumen)

- `GET /v1/health`
- `GET /v1/state?since=<state_version>`
- `POST /v1/actions/zfs` (operaciones mutables autorizadas)
- `POST /v1/admin/reconcile`
- `POST /v1/admin/resync`

Todos los endpoints con mTLS + autorización.

## Despliegue automático desde ZFSMgr

### Detección

- `agent present?`
- `agent running?`
- `api version compatible?`
- `cert valid?`

### Si falla cualquiera

- instalar/actualizar agent,
- registrar servicio del SO,
- arrancar y validar.

### Compatibilidad progresiva

- si agent no puede desplegarse, fallback temporal al modo SSH clásico (con aviso).

## Plan de implementación por fases

Fase 1
- Linux daemon + mTLS + bootstrap + lectura state inicial + API read-only.

Fase 2
- Ingesta zevents + reconciliación + deltas por `state_version`.

Fase 3
- Operaciones mutables (snapshot, destroy, send/recv, propiedades) vía API.

Fase 4
- FreeBSD port.

Fase 5
- macOS port (fuente de eventos equivalente cuando aplique).

Fase 6
- Windows port (servicio + estrategia de eventos alternativa).

## Estado de implementación (rama `daemonize`)

Implementado actualmente:

- detección de daemon por conexión en `refresh`:
  - instalado/activo/version/api/scheduler/detail
- señalización de atención en UI (`(*)`) cuando daemon requiere acción
- nodo `Daemon` bajo `Info` con estado y razones de atención
- menú contextual de conexión:
  - instalar/actualizar daemon
  - desinstalar daemon
- bootstrap inicial automático (con confirmación) al conectar cuando falta daemon
- bootstrap Unix con material TLS local:
  - creación de `server.crt` y `server.key` en `/etc/zfsmgr/tls` si no existen
- `zfsmgr-agent --serve` operativo como daemon residente TLS:
  - escucha TCP local (`127.0.0.1:47653` por defecto; configurable en `agent.conf`)
  - cifrado TLS con `server.crt/server.key`
  - mTLS local con certificado cliente dedicado (`client.crt/client.key`) y validación mutua
  - API JSON line-based interna
- `health` endurecido:
  - `--health` falla si no hay daemon residente alcanzable (`STATUS=DOWN`, `rc!=0`)
  - con daemon activo devuelve métricas de runtime (`SERVER=1`, `CACHE_ENTRIES`, `ZED_ACTIVE`, `ZED_RESTARTS`, `ZED_LAST_EVENT_UTC`)
- modo cliente transparente:
  - las invocaciones `--dump-*` intentan primero hablar con el daemon residente
  - si falla TLS/socket, hacen fallback automático a ejecución directa local
- caché en memoria en daemon residente (TTL rápido/lento configurable)
- invalidación reactiva de caché por eventos (`zpool events -f`)
- reconciliación periódica de seguridad (timer) que invalida caché aunque no lleguen eventos
- optimización del servidor residente:
  - la mayoría de lecturas `--dump-*` se ejecutan in-process en el daemon sin auto-spawn del binario
  - reducción de dependencia de `sh -lc` en lecturas clave (import probe, GSA scan, refresh basics, version, get/list JSON, batch guid/status)
  - watcher de eventos ZED lanzado de forma tipada (`zpool events -f`) sin shell wrapper
- limpieza de rutas duplicadas:
  - eliminado bloque legacy local de `--dump-*`; las lecturas pasan por un único camino (proxy TLS + fast-path tipado)

Pendiente de esta fase:

- reemplazar fallback por RPC tipado completo (sin spawn interno por petición)

## Riesgos clave

- divergencia silenciosa de caché si se confía solo en eventos,
- complejidad de bootstrap seguro multiplaforma,
- gestión de PKI/certificados en entornos heterogéneos,
- diferencias de OpenZFS por plataforma/versión.

## Recomendación final

Sí tiene sentido avanzar al modelo daemonizado, pero con estas condiciones:

- seguridad por mTLS y artefactos firmados,
- bootstrap por SSH solo para provisión,
- modelo híbrido eventos + reconciliación,
- fallback controlado a modo clásico mientras maduran los ports.

## Fuentes (verificación técnica)

- OpenZFS `zed(8)`:
  - https://openzfs.github.io/openzfs-docs/man/master/8/zed.8.html
- OpenZFS `zpool-events(8)`:
  - https://openzfs.github.io/openzfs-docs/man/master/8/zpool-events.8.html
- OpenZFS `zfs-events(5)` (nota de cobertura histórica/documentación de eventos):
  - https://openzfs.github.io/openzfs-docs/man/v2.0/5/zfs-events.5.html
- OpenZFS `zfs(8)` y `zpool(8)` (historial persistente de subcomandos que modifican estado):
  - https://openzfs.github.io/openzfs-docs/man/master/8/zfs.8.html
  - https://openzfs.github.io/openzfs-docs/man/master/8/zpool.8.html
