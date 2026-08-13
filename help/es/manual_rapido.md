# Manual rapido

ZFSMgr gestiona conexiones y acciones ZFS desde un árbol unificado.

## Vista general

![Ventana principal](qrc:/help/img/auto/main-window.png)

- Zona superior: un único árbol unificado que ocupa todo el ancho y casi todo el alto.
- Banda central, de **una sola línea**: `Origen`, `Estado` y `Progreso`.
- Zona inferior: pestañas (`Cambios pendientes`, `Ajustes`, `Log combinado`, `Terminal`,
  `Daemon`, `Transferencias`).

La caja `Acciones` con sus seis botones ya no existe: `Copiar`, `Mover`, `Clonar`,
`Sincronizar`, `Nivelar` y `Diff` se piden desde el menú contextual del nodo destino
(ver `Menús contextuales`). `Cambios pendientes` pasó a ser la primera pestaña de abajo.

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
- Si una conexión necesita atención del daemon, su nombre aparece con `(*)`.
- Si daemon-rpc entra en backoff por TLS, el motivo aparece temporalmente en el nodo de conexión y ZFSMgr intenta actualizar el daemon y recachear TLS automáticamente.
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

## Cambios pendientes

- `Cambios pendientes` muestra descripciones legibles, no comandos crudos.
- **El título de la pestaña lleva la cuenta entre paréntesis y en negrita** cuando hay
  algo pendiente, para que no pase inadvertido con la pestaña sin mirar.
- Los cambios se acumulan en orden de inserción.
- Al hacer clic en una línea, ZFSMgr intenta enfocar el objeto y la sección afectada.
- Acciones diferidas típicas:
  - cambios de propiedades
  - permisos
  - `Rename`, `Move`, `Rollback`, `Hold`, `Release`
  - `Copy`, `Level`, `Sync`
  - borrado diferido de datasets/snapshots

### La lista es un plan de trabajo, no una cola que se vacía

Las **acciones** (`Desglosar`, `Ensamblar`, `Desde Dir`, `Hacia Dir`, montar, desmontar,
crear, borrar…) se comportan así:

- **No se borran al ejecutarse.** Se quedan con su resultado y **se desmarcan solas**.
  Desmarcarlas, en vez de borrarlas, evita que un segundo `Aplicar cambios` repita por
  descuido un `Desglosar` o un `Hacia Dir` con borrado.
- **Casilla `Activa`**: decide si la entrada entra en el próximo `Aplicar cambios`.
  Volver a marcarla es todo lo que hace falta para repetir la acción.
- **La lista sobrevive al cierre.** Si sale sin aplicar, al arrancar siguen ahí.
- **`Poner nombre...`** (menú contextual) para distinguir entradas parecidas.
- **`Editar...`** (menú contextual) reabre el diálogo con lo que se pidió, en las cuatro
  acciones avanzadas. Cancelar la edición **no** borra la acción.
- Para quitar una entrada hay que hacerlo **a mano**: `Eliminar`, o `Vaciar lista` para
  descartarlo todo, que pide confirmación y enumera lo que se lleva.
- Una acción encolada guarda la ORDEN ya construida, así que no incorpora los arreglos
  posteriores del programa. Si la encoló otra versión, la fila sale con **⚠** y se
  pregunta antes de ejecutarla: lo seguro es quitarla y volver a pedir la acción.

Las **propiedades, los permisos y los renombrados** no funcionan así: siguen
desapareciendo al aplicarse. Son ediciones de un estado, con un final natural, no
trabajos que tenga sentido repetir.

### Qué NO se guarda en disco

- **Las contraseñas.** La orden de cada acción lleva dentro la de `sudo`; al guardar se
  sustituye por un marcador y al cargar se repone desde la conexión, donde vive cifrada.
  Si cambia la contraseña entre sesiones, la acción restaurada usa la nueva.
- **Las frases de cifrado.** Una acción que cree un dataset cifrado **no se guarda**:
  guardarla sin el secreto sería peor, porque al aplicarla crearía el dataset sin cifrar
  o fallaría a mitad. Al re-editarla, la frase se vuelve a pedir.
- Una acción cuya **conexión ya no existe** se descarta al arrancar, en vez de quedarse
  como una línea que falla al pulsarla.

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
