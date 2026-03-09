# Manual rapido

ZFSMgr gestiona conexiones y acciones ZFS.

- Panel izquierdo:
- `Conexiones`: listado simple (una fila por conexion).
- `Nuevo`: botones `Conexión`, `Pool` y `Refrescar Todo`.
- `Acciones`: operaciones de transferencia y avanzadas con origen/destino.
- Panel derecho:
- Al seleccionar una conexion: tab `Conexión <nombre>`.
- En esa conexion: propiedades de conexión (arriba) y estado de conexión (abajo).
- Al seleccionar una conexion con pools importados: un tab por pool importado.
- Subtab `Propiedades <pool>`: propiedades del pool (arriba) y estado del pool (abajo).
- Subtab `Contenido <pool>`: árbol de datasets/snapshots (arriba) y propiedades del dataset/snapshot (abajo).
- Los pools no importables no tienen tab; aparecen en el estado de la conexión con su motivo.
- Logs: panel único `Log combinado` (incluye salida SSH/PSRP con prefijo de conexión).

Comportamiento de navegación:

- Cambiar de conexión/tab reutiliza caché de datos.
- No se refresca automáticamente por solo navegar.
- Se refresca al ejecutar acciones que modifican estado/datos o al refresco explícito.

Revise "Atajos y estados" para criterios de habilitación de acciones.
