# Accion: Sincronizar

Objetivo: sincronizar contenido de dataset origen hacia dataset destino.

Condiciones:

- Origen: dataset seleccionado.
- Destino: dataset seleccionado.
- Origen y destino deben ser distintos.

Comportamiento:

- Usa `rsync` o `tar` segun plataforma y conexion.
- Muestra progreso (MB/GB transferidos) en log.
- Respeta cancelacion.

