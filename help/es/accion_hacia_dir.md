# Accion: Hacia Dir

Objetivo: copiar el contenido de un dataset a un directorio en la maquina de la conexion seleccionada.

Condiciones:

- **No está disponible en conexiones Windows**: el agente de allí no sirve este verbo,
  porque apartar el dataset a un montaje temporal —lo que exige convertirlo en un
  directorio en su mismo sitio— no se puede hacer en Windows.
- Dataset seleccionado en el árbol del pool.
- La seleccion debe ser dataset (no snapshot).
- En conexiones Unix requiere el agente `zfsmgr-agent` instalado: no hay alternativa por shell.

Comportamiento:

- Abre una ventana para seleccionar el directorio destino.
- **Se ejecuta al pulsarla.** Antes se añadía a una lista de cambios pendientes y esperaba
  a que usted la aplicara; esa lista ya no existe.
- Reubica el dataset en un punto de montaje temporal y copia su contenido con la copia propia del agente (permisos, enlaces duros, ficheros dispersos, atributos extendidos y, con ellos, las ACL POSIX). Reubicarlo permite que el directorio destino sea la propia ruta donde estaba montado.
- Si la copia termina correctamente:
  - opcionalmente elimina el dataset origen (segun check).
- Registra el comando y el resultado (`[TODIR] ok`) en el log combinado.

Advertencias importantes:

- **El contenido NO se fusiona**: se copia primero a un área temporal y se intercambia al final, así que el directorio destino queda con el contenido anterior o con el nuevo, nunca a medias. El anterior se aparta como respaldo y se restaura si algo falla.
- Se **rechaza** un directorio destino que ya sea punto de montaje ZFS.
- La copia preserva **atributos extendidos y ACL POSIX** en Linux y macOS. En FreeBSD no: usa otra interfaz para los atributos y no está portada.
- **Si no se elimina el dataset, este queda montado** en su punto de montaje habitual. Conviene comprobar que no se solapa con el directorio destino.
- **Sí hay verificación antes de borrar nada**: al terminar de copiar se cuenta lo que falta en el destino y se reintenta hasta cuatro veces; si aún queda algo pendiente, la acción se detiene con `verify_failed` y no destruye el dataset.
- No hay líneas de progreso en tiempo real: la salida aparece al terminar.
