# Accion: Hacia Dir

Objetivo: copiar el contenido de un dataset a un directorio en la maquina de la conexion seleccionada.

Condiciones:

- Dataset seleccionado en `Contenido <pool>`.
- La seleccion debe ser dataset (no snapshot).

Comportamiento:

- Abre una ventana para seleccionar el directorio destino.
- Copia el contenido del dataset al directorio preservando metadatos/permisos.
- Si la copia/verificacion termina correctamente:
  - opcionalmente elimina el dataset origen (segun check).
- Si no se elimina el dataset, queda desmontado para evitar solape con el directorio.
- Registra comandos y progreso en el log combinado.
