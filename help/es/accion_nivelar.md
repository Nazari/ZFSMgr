# Accion: Nivelar

Objetivo: alinear estado entre origen y destino usando snapshot/dataset.

Condiciones:

- Origen: dataset o snapshot.
- Destino: dataset.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Comportamiento:

- Calcula comando segun seleccion y estado remoto.
- Ejecuta transferencia con validaciones previas.
- Si ambas conexiones tienen daemon activo con soporte de jobs (`JOBS_SUPPORT=1`), la transferencia se lanza como **job en background**:
  - Los datos fluyen directamente de daemon a daemon, sin pasar por la máquina donde corre ZFSMgr.
  - La GUI no se bloquea; el progreso se muestra en la pestaña **Transferencias**.
  - Se puede cerrar la GUI mientras la transferencia continúa en el daemon.
  - Los jobs pueden cancelarse desde la pestaña Transferencias.
- Si algún daemon no soporta jobs, la acción cae en modo síncrono: se añade a `Cambios pendientes` y se ejecuta al aplicar los cambios.
- Registra subcomandos en nivel INFO.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.
