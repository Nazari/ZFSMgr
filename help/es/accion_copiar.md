# Accion: Copiar

Objetivo: enviar un snapshot desde origen y recibirlo en destino.

Condiciones:

- Origen: snapshot seleccionado.
- Destino: dataset seleccionado.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Comportamiento:

- Usa `zfs send` y `zfs recv`.
- Muestra progreso en el log combinado.
- Al finalizar, refresca la conexion destino.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.
