# Accion: Hacia Dir

Objetivo: copiar el contenido de un dataset a un directorio en la maquina de la conexion seleccionada.

Condiciones:

- Dataset seleccionado en el árbol del pool.
- La seleccion debe ser dataset (no snapshot).
- En conexiones Unix requiere el agente `zfsmgr-agent` instalado: no hay alternativa por shell.

Comportamiento:

- Abre una ventana para seleccionar el directorio destino.
- Muestra el comando, pide confirmación y se ejecuta en el acto, bloqueando la interfaz mientras dura. No pasa por `Cambios pendientes`.
- Monta el dataset y copia su contenido al directorio con `rsync -aHWS` (permisos, enlaces duros y ficheros dispersos).
- Si la copia termina correctamente:
  - opcionalmente elimina el dataset origen (segun check).
- Registra el comando y el resultado (`[TODIR] ok`) en el log combinado.

Advertencias importantes:

- **El contenido se fusiona con lo que ya hubiera en el directorio destino.** El directorio se crea si no existe, pero no se hace copia de respaldo previa ni se revierte nada si la copia falla a medias.
- **No se comprueba si el directorio destino es ya un punto de montaje ZFS.**
- **No se preservan ACLs ni atributos extendidos** (`rsync` se ejecuta sin `-A` ni `-X`).
- **Si no se elimina el dataset, este queda montado** en su punto de montaje habitual. Conviene comprobar que no se solapa con el directorio destino.
- No hay verificación posterior de la copia: el resultado se decide por el código de salida de `rsync`.
- No hay líneas de progreso en tiempo real: la salida aparece al terminar.
