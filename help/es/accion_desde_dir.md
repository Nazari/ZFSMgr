# Accion: Desde Dir

Objetivo: crear un dataset hijo a partir de un directorio existente en la maquina de la conexion seleccionada.

Condiciones:

- Dataset seleccionado en `Contenido <pool>`.
- La seleccion debe ser dataset (no snapshot).

Comportamiento:

- Abre una ventana para definir el nuevo dataset y seleccionar el directorio origen.
- Crea el dataset con montaje temporal seguro.
- Copia contenido preservando metadatos/permisos.
- Si la copia/verificacion termina correctamente:
  - opcionalmente borra el directorio origen (segun check),
  - deja el dataset desmontado para evitar pisar el directorio local si no se borra.
- Registra comandos y progreso en el log combinado.
