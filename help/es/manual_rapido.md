# Manual rapido

ZFSMgr gestiona conexiones y acciones ZFS desde un árbol unificado.

## Vista general

![Ventana principal](qrc:/help/img/auto/main-window.png)

- Zona superior: un único árbol unificado que ocupa todo el ancho.
- Zona central:
  - línea `Estado` y `Progreso`
  - caja `Acciones` (incluye `Source` y `Target` en una sola línea)
  - caja `Cambios pendientes` a la derecha de `Acciones`
- Zona inferior: pestañas de logs (`Ajustes`, `Log combinado`, `Terminal`, `GSA`).

## Árbol unificado

- Referencia visual del árbol:

![Árbol unificado](qrc:/help/img/auto/top-tree.png)

- Las conexiones aparecen siempre como nodos raíz, incluso si están desconectadas.
- Si una conexión está desconectada:
  - la conexión sigue visible
  - no muestra hijos (ni siquiera nodos auxiliares)
- En el nombre de conexión se muestra el modo activo:
  - `(libzfs_core)` cuando el daemon remoto está activo
  - `(ssh)` en fallback
- Si una conexión necesita atención GSA, su nombre aparece con `(*)`.
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
    - `GSA`
    - `Commands`

- Las propiedades inline pueden editarse directamente en el árbol.
- Si una propiedad admite herencia, aparece `Inh.` y el borrador se acumula sin ejecutar inmediatamente.
- `Permisos` también trabaja en modo borrador.
- `Datasets programados` usa propiedades `org.fc16.gsa:*`.

## Selección de origen y destino

- Ya no existe selección `Origen/Destino` por checks en una tabla.
- Para elegirlos:
  - clic derecho sobre un dataset
  - `Seleccionar como origen`
  - `Seleccionar como destino`
- La línea `Source/Target` de la caja `Acciones` refleja esa selección lógica.
- La selección visual actual del árbol y las selecciones lógicas `Origen/Destino` son independientes.

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

## Cambios pendientes

- `Pending changes` muestra descripciones legibles, no comandos crudos.
- Los cambios se acumulan en orden de inserción.
- Al hacer clic en una línea, ZFSMgr intenta enfocar el objeto y la sección afectada.
- Acciones diferidas típicas:
  - cambios de propiedades
  - permisos
  - `Rename`, `Move`, `Rollback`, `Hold`, `Release`
  - `Copy`, `Level`, `Sync`
  - borrado diferido de datasets/snapshots

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
