# Accion: Ensamblar

Objetivo: convertir subdatasets en directorios dentro del dataset padre.

Condiciones:

- Dataset seleccionado en el árbol del pool.
- El dataset padre debe estar **montado**; si no lo está, la operación falla con `mountpoint=none`. Cada subdataset seleccionado lo monta el propio agente.
- En conexiones Unix requiere el agente `zfsmgr-agent` instalado: no hay alternativa por shell.

Comportamiento:

- Muestra una pantalla para seleccionar subdatasets. El listado es recursivo: incluye descendientes anidados, no solo los hijos directos.
- Muestra el comando, pide confirmación y se ejecuta en el acto, bloqueando la interfaz mientras dura.
- Para cada subdataset: copia su contenido a un directorio temporal en `/tmp` del sistema remoto, destruye el subdataset (`zfs destroy -r`) y copia el contenido desde el temporal al directorio correspondiente dentro del padre.
- El subdataset solo se destruye si la copia **al directorio temporal** ha terminado correctamente.
- Registra en NORMAL los subdatasets seleccionados antes de empezar y una línea `[ASSEMBLE] ok` por subdataset al terminar. No hay progreso en tiempo real durante la copia.

Advertencias importantes:

- **`zfs destroy -r` arrastra también los descendientes** del subdataset elegido.
- **Se usa `/tmp` del sistema remoto como almacenamiento intermedio**: debe haber espacio libre suficiente.
- La copia preserva **ACLs y atributos extendidos** cuando el `rsync` del sistema los soporta (se detecta automáticamente).
- Antes de destruir el dataset de origen se **verifica que la copia esté completa**; si falta algo, la acción se detiene y el dataset **no se destruye**.
