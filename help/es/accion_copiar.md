# Accion: Copiar

Objetivo: enviar un snapshot desde origen y recibirlo en destino.

Condiciones:

- Origen: snapshot seleccionado.
- Destino: dataset seleccionado.

Comportamiento:

- Usa `zfs send` y `zfs recv`.
- Muestra progreso en el log combinado.
- Al finalizar, refresca la conexion destino.

