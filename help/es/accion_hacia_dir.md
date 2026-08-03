# Accion: Hacia Dir

Objetivo: copiar el contenido de un dataset a un directorio en la maquina de la conexion seleccionada.

Condiciones:

- Dataset seleccionado en el árbol del pool.
- La seleccion debe ser dataset (no snapshot).
- En conexiones Unix requiere el agente `zfsmgr-agent` instalado: no hay alternativa por shell.

Comportamiento:

- Abre una ventana para seleccionar el directorio destino.
- Muestra el comando, pide confirmación y se ejecuta en el acto, bloqueando la interfaz mientras dura. No pasa por `Cambios pendientes`.
- Reubica el dataset en un punto de montaje temporal y copia su contenido con `rsync` (permisos, enlaces duros, ficheros dispersos y, si el sistema lo soporta, ACLs y atributos extendidos). Reubicarlo permite que el directorio destino sea la propia ruta donde estaba montado.
- Si la copia termina correctamente:
  - opcionalmente elimina el dataset origen (segun check).
- Registra el comando y el resultado (`[TODIR] ok`) en el log combinado.

Advertencias importantes:

- **El contenido NO se fusiona**: se copia primero a un área temporal y se intercambia al final, así que el directorio destino queda con el contenido anterior o con el nuevo, nunca a medias. El anterior se aparta como respaldo y se restaura si algo falla.
- Se **rechaza** un directorio destino que ya sea punto de montaje ZFS.
- La copia preserva **ACLs y atributos extendidos** cuando el `rsync` del sistema los soporta (se detecta automáticamente).
- **Si no se elimina el dataset, este queda montado** en su punto de montaje habitual. Conviene comprobar que no se solapa con el directorio destino.
- No hay verificación posterior de la copia: el resultado se decide por el código de salida de `rsync`.
- No hay líneas de progreso en tiempo real: la salida aparece al terminar.
