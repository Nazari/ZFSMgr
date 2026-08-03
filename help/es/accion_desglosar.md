# Accion: Desglosar

Objetivo: crear subdatasets a partir de directorios del dataset padre.

Condiciones:

- Dataset seleccionado en el árbol del pool.
- El dataset debe estar **montado**: si no lo está, no se listará ningún directorio candidato.
- En conexiones Unix requiere el agente `zfsmgr-agent` instalado: no hay alternativa por shell.

Comportamiento:

- Muestra una pantalla para seleccionar directorios.
- Se ocultan los directorios que ya corresponden a subdatasets o que contienen puntos de montaje de descendientes, y se marcan como no seleccionables los nombres no válidos como dataset (con `@`, `#`, `,`, `.`, `..` o caracteres de control).
- Muestra el comando, pide confirmación y se ejecuta en el acto, bloqueando la interfaz mientras dura.
- Para cada directorio: crea el subdataset montado en un directorio temporal de `/tmp`, copia el contenido, elimina el directorio origen y solo entonces reasigna el `mountpoint` del subdataset a la ruta original y lo remonta.
- Registra en NORMAL los directorios seleccionados antes de empezar y una línea `[BREAKDOWN] ok` por directorio al terminar. No hay progreso en tiempo real durante la copia.

Advertencias importantes:

- **Se usa `/tmp` del sistema remoto como almacenamiento intermedio**: debe haber espacio libre suficiente para el contenido de cada directorio.
- Antes de borrar el directorio origen se **verifica que la copia esté completa** (pasada en seco de `rsync` contando ficheros pendientes, con reintentos). Si tras varios intentos falta algo, la acción se detiene y **no borra nada**.
- La copia preserva **ACLs y atributos extendidos** cuando el `rsync` del sistema los soporta (se detecta automáticamente).
