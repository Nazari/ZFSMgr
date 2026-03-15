# Manual rapido

ZFSMgr gestiona conexiones y acciones ZFS.

- Panel izquierdo:
- `Conexiones`: tabla simple (una fila por conexión) con checks `Origen` y `Destino`.
- `Acciones`: operaciones de transferencia y avanzadas.
  Incluye `Copiar`, `Clonar`, `Diff`, `Nivelar`, `Sincronizar`, `Desglosar`, `Ensamblar`, `Desde Dir` y `Hacia Dir`.
- Panel derecho:
- Arriba: árbol de contenido de la conexión marcada como `Origen`.
- Abajo: árbol de contenido de la conexión marcada como `Destino`.
- La selección efectiva del detalle no depende del clic en la fila, sino de los checks:
  - `Origen` controla el árbol superior.
  - `Destino` controla el árbol inferior.
- Cada árbol mantiene su propio estado de navegación por conexión/pool:
  - expansión/colapso,
  - selección de dataset,
  - snapshot seleccionado,
  - anchos de columnas.
- Cada árbol muestra el pool activo de su conexión seleccionada.
- Bajo cada pool aparecen los nodos `Información`, `Capacidades activas` y `Contenido`.
- El nodo `Información` del pool muestra propiedades de pool inline.
- Los datasets cuelgan de `Contenido`.
- Cada dataset muestra siempre un nodo `Propiedades` inicialmente colapsado y, si tiene hijos, `Subdatasets (N)`.
- Cuando hay un snapshot seleccionado en un dataset, ese árbol muestra las propiedades y grupos del snapshot, y aparece además `Holds (N)`.
- Las propiedades inline pueden incluir edición directa y control de herencia (`Inh.`) cuando aplica.
- Los pools no importables también aparecen como nodo raíz para permitir `Importar`.
- Logs: panel único `Log combinado` (incluye salida SSH/PSRP con prefijo de conexión).
- El encabezado de cada treeview tiene menú contextual para ajustar una columna o todas al contenido.
- El scroll vertical de los treeviews es suave, por píxel.
- El botón `Aplicar cambios` solo se activa si hay comandos pendientes reales y su tooltip lista esos comandos.
- `Diff` muestra sus resultados en una ventana de árbol con `Añadido`, `Borrado`, `Modificado` y `Renombrado`.

Comportamiento de navegación:

- Cambiar de conexión/pool reutiliza caché de datos.
- No se refresca automáticamente por solo navegar.
- Se refresca al ejecutar acciones que modifican estado/datos o con refresco explícito.
- Antes de cada acción se guarda/restaura el estado visual de ambos árboles (selección y expansión de nodos, cuando aplica).
- Si ningún check `Origen`/`Destino` está activo para un árbol, ese árbol queda vacío pero conserva cabeceras coherentes.
- `Origen` y `Destino` se persisten entre ejecuciones.

Revise `Navegación y estados` y `Propiedades inline y columnas` para detalles de funcionamiento.
