# Manual rapido

ZFSMgr gestiona conexiones y acciones ZFS.

- Panel izquierdo:
- `Conexiones`: tabla simple (una fila por conexión) con checks `Origen` y `Destino`.
- `Acciones`: operaciones de transferencia y avanzadas.
- Panel derecho:
- Arriba: tabs por pool de la conexión marcada como `Origen`.
- Abajo: tabs por pool de la conexión marcada como `Destino`.
- Cada tab de pool muestra un único árbol `Contenido`, con nodo raíz `Pool` y datasets/snapshots.
- El nodo `Pool` muestra información de pool; los nodos dataset/snapshot muestran propiedades editables en columnas `Prop.`.
- Los pools no importables no tienen tab; aparecen en el tooltip/estado de la conexión con su motivo.
- Logs: panel único `Log combinado` (incluye salida SSH/PSRP con prefijo de conexión).

Comportamiento de navegación:

- Cambiar de conexión/tab reutiliza caché de datos.
- No se refresca automáticamente por solo navegar.
- Se refresca al ejecutar acciones que modifican estado/datos o con refresco explícito.
- Antes de cada acción se guarda el tab activo de pool (arriba y abajo) y se restaura al finalizar.

Revise "Atajos y estados" para criterios de habilitación de acciones.
