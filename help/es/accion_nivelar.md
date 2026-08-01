# Accion: Nivelar

Objetivo: alinear estado entre origen y destino usando snapshot/dataset.

Condiciones:

- Origen: dataset o snapshot.
- Destino: dataset.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Requisitos de nivelación (si no se cumplen, la acción se cancela con un aviso):

- Nivelar es **siempre diferencial** (`zfs send -I`), desde el snapshot más reciente del destino hasta el snapshot objetivo del origen.
- El destino debe tener **al menos un snapshot**.
- El snapshot más reciente del destino debe existir también en el origen. La correspondencia se establece **por GUID, no por nombre**: dos snapshots con el mismo nombre pero distinto GUID no sirven.
- Ese snapshot del destino no puede ser más moderno que el snapshot objetivo del origen.
- Si origen y destino ya coinciden, se informa de que ya está nivelado y no se hace nada.

Comportamiento:

- Antes de encolar se abre un diálogo con las opciones de `zfs send`.
- Si el destino tiene daemon activo con soporte de jobs (`JOBS_SUPPORT=1`) —y también el origen, cuando son conexiones distintas— y ninguna de las dos es Windows, la transferencia se lanza como **job en background**:
  - Los datos fluyen directamente de daemon a daemon, sin pasar por la máquina donde corre ZFSMgr.
  - La GUI no se bloquea; el progreso se muestra en la pestaña **Transferencias**.
  - Se puede cerrar la GUI mientras la transferencia continúa en el daemon.
  - Los jobs pueden cancelarse desde la pestaña Transferencias.
- Si no hay soporte de jobs, la acción cae en modo síncrono: se añade a `Cambios pendientes` y se ejecuta al aplicar los cambios.
- Con origen y destino en la **misma conexión**, la tubería `zfs send | zfs recv` la monta el propio daemon (`--zfs-pipe-local`), sin shell remoto. En esa ruta **no se muestran líneas de progreso**; el avance solo es visible en la ruta de jobs.
- Registra el modo de transferencia elegido en nivel INFO y el cambio pendiente en nivel NORMAL.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.
