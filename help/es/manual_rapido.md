# Manual rapido

ZFSMgr gestiona conexiones y acciones ZFS.

- Panel izquierdo:
- `Conexiones`: listado simple (una fila por conexion).
- `Nuevo`: crear conexion o crear pool.
- `Acciones`: operaciones de transferencia y avanzadas.
- Panel derecho:
- Al seleccionar una conexion: tab `Conexion <nombre>`.
- Al seleccionar una conexion con pools visibles: un tab por pool importado o importable.
- `Propiedades <pool>`: propiedades del pool (arriba) y estado del pool (abajo).
- `Contenido <pool>`: contenido del dataset (arriba) y propiedades del dataset (abajo).
- Los pools no importables no tienen tab; aparecen en el estado de la conexion con su motivo.
- Logs: panel único `Log combinado` (incluye salida SSH/PSRP con prefijo de conexion).

Revise "Atajos y estados" para criterios de habilitacion de botones.
