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
- La primera columna del árbol superior se identifica siempre como `Origen:...` y la del inferior como `Destino:...`.
- Cada árbol muestra el pool activo de su conexión seleccionada.
- Bajo cada pool puede aparecer el nodo `Información del pool`.
- El nodo `Información del pool` muestra propiedades de pool inline.
- El menú contextual del pool incluye `Actualizar`, `Importar`, `Exportar`, `Historial`, `Sync`, `Scrub`, `Trim`, `Initialize` y `Destroy` según el estado del pool.
- Los datasets cuelgan directamente del pool.
- Los subdatasets cuelgan directamente de su dataset padre.
- Cada dataset puede mostrar un nodo `Propiedades` inicialmente colapsado.
- En datasets no snapshot también puede aparecer `Permisos`, para revisar y modificar delegaciones ZFS.
- Dentro de `Permisos` aparecen:
  - `Deleg.`
  - `Nuevos DS`
  - `Conjuntos`
- Los checks de `Permisos` trabajan en modo borrador.
  Los cambios se acumulan y se aplican junto con `Aplicar cambios`.
- Dentro de `Permisos`, el bloque `Nuevos DS` define qué permisos recibirá automáticamente quien cree descendientes nuevos bajo ese dataset.
- Cuando hay un snapshot seleccionado en un dataset, ese árbol muestra las propiedades y grupos del snapshot, y aparece además `Holds (N)`.
- Las propiedades inline pueden incluir edición directa y control de herencia (`Inh.`) cuando aplica.
- Si una propiedad no está soportada por el sistema operativo de la conexión, aparece atenuada y no se puede editar.
  Ejemplos: `sharesmb` en macOS, `jailed` fuera de FreeBSD, `zoned`/`nbmand` fuera de Linux.
- Los pools no importables también aparecen como nodo raíz para permitir `Importar`.
- Logs: panel único `Log combinado` (incluye salida SSH/PSRP con prefijo de conexión).
- La zona inferior del log usa pestañas:
  - `Aplicación` para el log textual
  - `Cambios pendientes` para los comandos diferidos
- El encabezado de cada treeview tiene menú contextual para ajustar una columna o todas al contenido y para cambiar `Columnas de propiedades`.
- El scroll vertical de los treeviews es suave, por píxel.
- El menú contextual del árbol permite además mostrar u ocultar `Información del pool`, `Propiedades` y `Permisos` inline.
- El botón `Aplicar cambios` solo se activa si hay comandos pendientes reales.
- `Cambios pendientes` muestra un cambio por línea, con prefijo `conexión::pool`.
- Los cambios pendientes se conservan en orden de ejecución.
- Al hacer clic en una línea de `Cambios pendientes`, ZFSMgr intenta llevar el foco al dataset y sección afectados.
  Si el pool está visible en ambos árboles, se prioriza `Origen`.
- `Diff` muestra sus resultados en una ventana de árbol con `Añadido`, `Borrado`, `Modificado` y `Renombrado`.

Comportamiento de navegación:

- Cambiar de conexión/pool reutiliza caché de datos.
- No se refresca automáticamente por solo navegar.
- Se refresca al ejecutar acciones que modifican estado/datos o con refresco explícito.
- Antes de cada acción se guarda/restaura el estado visual de ambos árboles.
  Cuando aplica, eso incluye selección, expansión de nodos y posición de scroll.
- Al pinchar un nodo `Propiedades` vacío, sus hijos se cargan y el nodo queda desplegado.
- Si cambia `Columnas de propiedades`, un nodo `Propiedades` ya desplegado conserva su expansión.
- Si ningún check `Origen`/`Destino` está activo para un árbol, ese árbol queda vacío pero conserva cabeceras coherentes.
- `Origen` y `Destino` se persisten entre ejecuciones.

Revise `Navegación y estados` y `Propiedades inline y columnas` para detalles de funcionamiento.
