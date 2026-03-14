# Manual rapido

ZFSMgr gestiona conexiones y acciones ZFS.

- Panel izquierdo:
- `Conexiones`: tabla simple (una fila por conexión) con checks `Origen` y `Destino`.
- `Acciones`: operaciones de transferencia y avanzadas.
  Incluye `Copiar`, `Clonar`, `Nivelar`, `Sincronizar`, `Desglosar`, `Ensamblar`, `Desde Dir` y `Hacia Dir`.
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
- Cada árbol puede mostrar varios pools a la vez (un nodo raíz por pool), con datasets/snapshots debajo.
- El nodo `Información` del pool muestra propiedades de pool inline.
- Los nodos dataset muestran propiedades inline distribuidas en columnas `C1..Cn`.
- Las propiedades inline pueden incluir edición directa y control de herencia (`Inh.`) cuando aplica.
- Los pools no importables también aparecen como nodo raíz para permitir `Importar`.
- Logs: panel único `Log combinado` (incluye salida SSH/PSRP con prefijo de conexión).

Comportamiento de navegación:

- Cambiar de conexión/pool reutiliza caché de datos.
- No se refresca automáticamente por solo navegar.
- Se refresca al ejecutar acciones que modifican estado/datos o con refresco explícito.
- Antes de cada acción se guarda/restaura el estado visual de ambos árboles (selección y expansión de nodos, cuando aplica).
- Si ningún check `Origen`/`Destino` está activo para un árbol, ese árbol queda vacío pero conserva cabeceras coherentes.

Revise `Navegación y estados` y `Propiedades inline y columnas` para detalles de funcionamiento.
