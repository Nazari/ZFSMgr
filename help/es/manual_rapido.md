# Manual rapido

ZFSMgr gestiona conexiones y acciones ZFS.

- Panel izquierdo:
- `Conexiones`: tabla simple (una fila por conexión) con checks `Origen` y `Destino`.
- `Acciones`: operaciones de transferencia y avanzadas.
  Incluye `Copiar`, `Clonar`, `Nivelar`, `Sincronizar`, `Desglosar`, `Ensamblar`, `Desde Dir` y `Hacia Dir`.
- Panel derecho:
- Arriba: árbol de contenido de la conexión marcada como `Origen`.
- Abajo: árbol de contenido de la conexión marcada como `Destino`.
- Cada árbol puede mostrar varios pools a la vez (un nodo raíz por pool), con datasets/snapshots debajo.
- El nodo `Pool` muestra información de pool; los nodos dataset/snapshot muestran propiedades editables en columnas `Prop.`.
- Los pools no importables también aparecen como nodo raíz para permitir `Importar`.
- Logs: panel único `Log combinado` (incluye salida SSH/PSRP con prefijo de conexión).

Comportamiento de navegación:

- Cambiar de conexión/pool reutiliza caché de datos.
- No se refresca automáticamente por solo navegar.
- Se refresca al ejecutar acciones que modifican estado/datos o con refresco explícito.
- Antes de cada acción se guarda/restaura el estado visual de ambos árboles (selección y expansión de nodos, cuando aplica).

Revise "Atajos y estados" para criterios de habilitación de acciones.
