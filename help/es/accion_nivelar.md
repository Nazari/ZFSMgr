# Accion: Nivelar

Objetivo: alinear estado entre origen y destino usando snapshot/dataset.

Condiciones:

- Origen: dataset o snapshot.
- Destino: dataset.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Comportamiento:

- Calcula comando segun seleccion y estado remoto.
- Ejecuta transferencia con validaciones previas.
- Si origen y destino son dos conexiones SSH remotas distintas, intenta transferir directamente de origen a destino sin pasar los datos por la máquina donde corre ZFSMgr.
- Registra subcomandos en nivel INFO.
- La acción se añade primero a `Cambios pendientes` y solo se ejecuta al aplicar los cambios.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.
