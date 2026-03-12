# Accion: Sincronizar

Objetivo: sincronizar contenido de dataset origen hacia dataset destino.

Condiciones:

- Origen: dataset seleccionado.
- Destino: dataset seleccionado.
- Origen y destino deben ser distintos.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Comportamiento:

- Usa `rsync` o `tar` segun plataforma y conexion.
- Muestra progreso (MB/GB transferidos) en log.
- Respeta cancelacion.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.
