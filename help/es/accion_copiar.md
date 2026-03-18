# Accion: Copiar

Objetivo: enviar un snapshot desde origen y recibirlo en destino.

Condiciones:

- Origen: snapshot seleccionado.
- Destino: dataset seleccionado.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Comportamiento:

- Usa `zfs send` y `zfs recv`.
- Si origen y destino son dos conexiones SSH remotas distintas, ZFSMgr intenta que la transferencia vaya directamente de origen a destino.
  En ese caso, el flujo de datos no pasa por la máquina donde corre ZFSMgr; esa máquina solo mantiene el control y recibe el progreso.
- Muestra progreso en el log combinado.
- La acción se añade primero a `Cambios pendientes` y solo se ejecuta al aplicar los cambios.
- Al finalizar, refresca las conexiones y árboles afectados.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.
