# Diseño y funcionamiento del GSA

## Estado vigente

En la arquitectura actual, el `Gestor de snapshots` (GSA) está integrado en el daemon nativo remoto (`zfsmgr-agent`).

No se gestiona como un componente independiente desde un menú específico de GSA. La gestión se hace desde el menú `Daemon` de cada conexión (instalar/actualizar, desinstalar, estado).

## Objetivo funcional

El GSA ejecuta snapshots automáticos por dataset y, cuando procede, nivelación (`zfs send | zfs recv`) hacia un destino.

La configuración se sigue expresando con propiedades ZFS `org.fc16.gsa:*` en cada dataset.

Propiedades principales:

- `org.fc16.gsa:activado`
- `org.fc16.gsa:recursivo`
- `org.fc16.gsa:horario`
- `org.fc16.gsa:diario`
- `org.fc16.gsa:semanal`
- `org.fc16.gsa:mensual`
- `org.fc16.gsa:anual`
- `org.fc16.gsa:nivelar`
- `org.fc16.gsa:destino`

## Modelo operativo

- Si se programa `B::pool/ds`, la ejecución periódica ocurre en `B`.
- El daemon de `B` evalúa vencimientos (`hourly/daily/weekly/monthly/yearly`).
- Crea snapshots `GSA-<clase>-YYYYMMDD-HHMMSS`.
- Aplica retención/poda por clase.
- Si `nivelar=on` y hay `destino`, intenta la ruta de nivelación.

## Programación recursiva

Si `recursivo=on` en un ancestro:

- se usa snapshot recursivo del ancestro,
- los descendientes quedan cubiertos,
- se omiten evaluaciones redundantes de hijos en la misma ejecución.

También se evita error si el snapshot objetivo ya existe: se registra `skip`.

## Destino de nivelación

Formato de `Destino`:

- `Conexion::pool/dataset`

Ejemplos:

- `BackupNAS::tank/copias/proyecto`
- `Local::backup/apps/app1`

## Señalización en UI

ZFSMgr marca la conexión con `(*)` cuando el daemon/scheduler requiere atención.

Casos típicos:

- daemon no instalado,
- daemon no activo,
- versión o API desalineada,
- incidencia de configuración requerida para scheduler/nivelación.

La visibilidad técnica está bajo `Info -> Daemon` en el árbol de conexión.

## Logs y observabilidad

El scheduler escribe eventos en `GSA.log` (ruta de configuración de ZFSMgr en cada host) y esos eventos también se exponen en la pestaña `GSA` de logs en la GUI.

Eventos típicos:

- inicio de ciclo (`GSA start version ...`),
- evaluación de dataset (`GSA evaluate ...`),
- creación de snapshot (`GSA snapshot created ...`),
- omisiones justificadas (`GSA skip ...`),
- nivelación (`GSA level ...`).

## Compatibilidad y fallback

- Si daemon-rpc está disponible, ZFSMgr prioriza ese canal.
- Si no está disponible, se usa fallback SSH controlado.
- La política de snapshots/nivelación sigue siendo la misma en ambos caminos.

## Referencias relacionadas

- [diseno_tecnico_daemon_nativo_zed.md](./diseno_tecnico_daemon_nativo_zed.md)
- [diseno_tecnico_endurecimiento_gsa.md](./diseno_tecnico_endurecimiento_gsa.md)
- [propuesta_endurecimiento_gsa.md](./propuesta_endurecimiento_gsa.md)
