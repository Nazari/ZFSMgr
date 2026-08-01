# Accion: Copiar

Objetivo: enviar un snapshot desde origen y recibirlo en destino.

Condiciones:

- Origen: snapshot seleccionado.
- Destino: dataset seleccionado.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Comportamiento:

- Usa `zfs send` y `zfs recv`. Antes de encolar se abre un diálogo con las opciones de `zfs send`.
- El destino real del `zfs recv` es `<dataset destino>/<nombre-hoja-del-origen>`, salvo que el destino ya termine con ese nombre.
- Si el destino tiene daemon activo con soporte de jobs (`JOBS_SUPPORT=1`) —y también el origen, cuando son conexiones distintas— y ninguna de las dos es Windows, la transferencia se lanza como **job en background**:
  - Los datos fluyen directamente de daemon a daemon, sin pasar por la máquina donde corre ZFSMgr.
  - La GUI no se bloquea; el job arranca inmediatamente y se puede seguir usando la aplicación.
  - El progreso se muestra en la pestaña **Transferencias** (volumen transferido, velocidad y tiempo).
  - Se puede cerrar la GUI mientras la transferencia sigue ejecutándose en el daemon remoto.
  - Cuando una conexión con daemon vuelve a estar activa, sus jobs en curso se recuperan automáticamente y reaparecen en la pestaña Transferencias.
  - Cada job puede cancelarse desde la pestaña Transferencias (envía `SIGTERM` al proceso `zfs send`). Solo puede cancelarse un job en ejecución.
- Si no hay soporte de jobs, la acción cae en modo síncrono: se añade a `Cambios pendientes` y se ejecuta al aplicar los cambios.
- Con origen y destino en la **misma conexión**, la tubería `zfs send | zfs recv` la monta el propio daemon (`--zfs-pipe-local`), sin shell remoto. En esa ruta **no se muestran líneas de progreso**; el avance solo es visible en la ruta de jobs.
- En conexiones Windows no hay jobs ni daemon: la copia se hace mediante un fallback `tar` sobre `.zfs/snapshot`, sin `zfs send`/`recv`.
- Al finalizar se refresca la conexión **destino** y su contenido.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.
