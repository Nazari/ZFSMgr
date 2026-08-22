# Manual rapido

ZFSMgr gestiona conexiones y acciones ZFS desde un árbol unificado.

## Vista general

![Ventana principal](qrc:/help/img/auto/main-window.png)

- Zona superior: un único árbol unificado que ocupa todo el ancho y casi todo el alto.
- Banda central, de **una sola línea**: `Origen`, `Estado` y `Progreso`.
- Zona inferior: pestañas (`Transferencias`, `Ajustes`, `Log combinado`,
  `Transferencias`). `Terminal` y `Daemon` no están aquí: son sub-pestañas **de cada
  conexión**, dentro del `Log combinado`.

La caja `Acciones` con sus seis botones ya no existe: `Copiar`, `Mover`, `Clonar`,
`Sincronizar`, `Nivelar` y `Diff` se piden desde el menú contextual del nodo destino
(ver `Menús contextuales`). `Transferencias` es la primera pestaña de abajo.

## Árbol unificado

- Referencia visual del árbol:

![Árbol unificado](qrc:/help/img/auto/top-tree.png)

- Las conexiones aparecen siempre como nodos raíz, incluso si están desconectadas.
- Si una conexión está desconectada:
  - la conexión sigue visible
  - no muestra hijos (ni siquiera nodos auxiliares)
- Si una conexión necesita atención del daemon, su nombre aparece con `(*)`, y el motivo
  se detalla en el nodo `Info` → `Daemon`.
- Si daemon-rpc queda en espera por un problema de TLS, el motivo aparece entre corchetes
  junto al nombre de la conexión. ZFSMgr **no** reinstala el daemon ni rehace el material
  TLS por su cuenta: lo marca y espera a que usted lo pida desde el menú contextual. Es
  deliberado — reaprovisionar solo porque un saludo TLS ha fallado puede dejar sin acceso
  una conexión que estaba bien.
- Los nodos `Conexión` y `Pool` se muestran en negrita y con prefijo de tipo.
- El nodo raíz del pool está fusionado con el dataset raíz del pool:
  - mantiene icono de pool
  - actúa también como dataset raíz
  - evita duplicar `pool/pool`
- En pools importados puede aparecer:
  - `Pool Information`
    - incluye `Dispositivos` (jerarquía de vdev/discos según `zpool status -P`)
  - `Datasets programados`
- Un pool en estado suspendido muestra `(Suspended)` junto a su nombre y bloquea la mayoría de sus operaciones.

## Nodos inline

- En datasets aparece `Dataset properties`.
- En snapshots aparece `Snapshot properties`.
- En datasets no snapshot también puede aparecer `Permisos`.
- En datasets con snapshots aparece el nodo `@`, que agrupa snapshots manuales y GSA.
- En conexiones aparecen nodos auxiliares:
  - `Propiedades de conexión` (inline, con permisos de edición por tipo de conexión)
  - `Info`
    - `General` (estado y metadatos de conexión)
    - `Daemon`
    - `Commands`

- Las propiedades inline pueden editarse directamente en el árbol.
- Si una propiedad admite herencia, aparece `Inh.` y el borrador se acumula sin ejecutar inmediatamente.
- `Permisos` también trabaja en modo borrador.
- `Datasets programados` usa propiedades `org.fc16.gsa:*`.

## Selección de origen y destino

- Solo se marca el **origen**: clic derecho sobre un dataset o snapshot → `Marcar como origen`.
- El **destino no se marca**. Es el nodo sobre el que se abre el menú contextual para pedir
  la acción, igual que al pegar.
- La línea `Origen:` de la banda superior recuerda lo marcado; su tooltip lo muestra
  completo cuando el nombre no cabe.
- La selección visual del árbol y el origen marcado son independientes.

## Menús contextuales

- Sobre una conexión:
  - aparece el antiguo menú contextual de conexiones
- Sobre el nodo raíz fusionado del pool:
  - aparece primero un submenú `Pool`
  - luego el resto de acciones de dataset
- El submenú `Pool` concentra acciones de pool:
  - `Refresh status`
  - `Importar`
  - `Importar renombrando`
  - `Exportar`
  - `Historial`
  - `Gestión`:
    - `Sync`
    - `Scrub`
    - `Upgrade`
    - `Reguid`
    - `Trim`
    - `Initialize`
    - `Clear`
    - `Destroy`
- En datasets/snapshots sigue habiendo acciones como:
  - `Crear dataset/snapshot/vol`
  - `Renombrar`
  - `Borrar`
  - `Encriptación`
  - `Programar snapshots automáticos`
  - `Rollback`
  - `Nuevo Hold`
  - `Release`
  - `Desglosar`
  - `Ensamblar`
  - `Desde Dir`
  - `Hacia Dir`

## Transferencias

Esta pestaña era `Cambios pendientes` y guardaba órdenes a la espera de que usted pulsara
`Aplicar cambios`. **Ya no.** Las acciones se ejecutan al pulsarlas, y la pestaña muestra
ahora los **trabajos en marcha**: qué está corriendo, su progreso, y un botón para
cancelarlo.

Lo que sí se sigue editando en lote son las **propiedades** y los **permisos**: se acumulan
como borradores y se aplican con `Aplicar cambios`. Son ediciones de un estado, con un final
natural; una acción como `Desglosar` o `Hacia Dir` no lo es.

Consecuencias de que la lista ya no exista:

- No hay nada que sobreviva al cierre de la aplicación: si no la ejecutó, no ocurrió. Antes
  la lista se guardaba en disco, con la orden de cada acción dentro.
- No hay que acordarse de aplicar nada. Antes una acción pedida y no aplicada parecía hecha.
- Los borradores de propiedades y permisos **sí** se pierden al cerrar sin aplicarlos.


## Conectividad y logs

- `Comprobar conectividad` está en el menú principal (no en `Logs`).
- El menú `Logs` se eliminó.
- La pestaña `Ajustes` concentra:
  - nivel de log
  - número de líneas
  - tamaño máximo de rotación
  - confirmar acciones
  - limpiar/copiar logs

## Creación de pools

![Crear pool](qrc:/help/img/crearpool.png)

- `Crear pool` abre el constructor de VDEV y parámetros del pool.
- La estructura del árbol del pool valida combinaciones OpenZFS compatibles.
- Si falla, el diálogo permanece abierto para corregir y reintentar.

## Creación de datasets

![Crear dataset](qrc:/help/img/creardataset.png)

- `Crear dataset` se abre desde el menú contextual del árbol.
- Si el dataset es cifrado con `keylocation=prompt`, ZFSMgr pide passphrase.
- Si falla, el diálogo permanece abierto con los datos introducidos.

## Paneles divididos (Split and root)

- El menú contextual de cualquier nodo de conexión, pool o dataset incluye `Split and root`.
- Al elegir una dirección (`Derecha`, `Izquierda`, `Abajo`, `Arriba`), se abre un nuevo panel de árbol junto al existente mediante un divisor.
- El nodo raíz del panel muestra la ruta completa (p. ej. `mbp::tank1/ds1/sub`).
- Los paneles divididos tienen plena funcionalidad: mismos menús contextuales, propiedades inline y columnas configurables.
- Los paneles pueden anidarse; cada uno tiene su propio menú de cabecera de columnas.
- Para cerrar un panel dividido: clic derecho sobre su nodo raíz → `Close`.
- La disposición de paneles se conserva entre sesiones.

## Navegación

- El árbol recuerda expansión, selección y snapshots seleccionados.
- Cambiar columnas de propiedades conserva la apertura de nodos visibles.
- Al pulsar un nodo vacío de propiedades, sus hijos se materializan y el nodo queda abierto.
