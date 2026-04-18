# Accion: Sincronizar

Objetivo: sincronizar contenido de dataset origen hacia dataset destino.

Condiciones:

- Origen: dataset seleccionado.
- Destino: dataset seleccionado.
- Origen y destino deben ser distintos, y ambos deben estar montados.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Comportamiento:

- Usa `rsync` o `tar` segun plataforma y conexion.
- La acción se añade primero a `Cambios pendientes` y solo se ejecuta al aplicar los cambios.
- Muestra progreso (MB/GB transferidos) en log.
- Respeta cancelacion.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.
