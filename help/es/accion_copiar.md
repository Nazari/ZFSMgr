# Accion: Copiar

Objetivo: enviar un snapshot desde origen y recibirlo en destino.

Condiciones:

- Origen: snapshot seleccionado.
- Destino: dataset seleccionado.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Comportamiento:

- Usa `zfs send` y `zfs recv`.
- Si ambas conexiones tienen daemon activo con soporte de jobs (`JOBS_SUPPORT=1`), la transferencia se lanza como **job en background**:
  - Los datos fluyen directamente de daemon a daemon, sin pasar por la máquina donde corre ZFSMgr.
  - La GUI no se bloquea; el job arranca inmediatamente y se puede seguir usando la aplicación.
  - El progreso se muestra en la pestaña **Transferencias** (bytes, velocidad, tiempo transcurrido).
  - Se puede cerrar la GUI mientras la transferencia sigue ejecutándose en el daemon remoto.
  - Al volver a abrir la GUI o reconectar, los jobs en curso se recuperan automáticamente.
  - Cada job puede cancelarse desde la pestaña Transferencias (envía `SIGTERM` al proceso `zfs send`).
- Si algún daemon no soporta jobs, la acción cae en modo síncrono: se añade a `Cambios pendientes` y se ejecuta al aplicar los cambios.
- Al finalizar, refresca las conexiones y árboles afectados.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.
