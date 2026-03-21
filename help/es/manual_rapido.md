# Manual rapido

ZFSMgr gestiona conexiones y acciones ZFS.

## Vista general

<img src="help-img/ventanaprincipal.png" alt="Pantalla principal" width="50%">

- Panel izquierdo:
- `Conexiones`: tabla simple (una fila por conexión) con columna `Conexión` y checks `O` y `D`.
- `Datasets seleccionados`: operaciones de transferencia y avanzadas.
  Incluye `Copiar`, `Clonar`, `Mover`, `Diff`, `Nivelar`, `Sincronizar`, `Desglosar`, `Ensamblar`, `Desde Dir` y `Hacia Dir`.
- Panel derecho:
- Arriba: árbol de contenido de la conexión marcada como `Origen`.
- Abajo: árbol de contenido de la conexión marcada como `Destino`.
- La selección efectiva del detalle no depende del clic en la fila, sino de los checks:
  - `O` controla el árbol superior (`Origen`).
  - `D` controla el árbol inferior (`Destino`).
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
- Si `Inh.=on`, el valor queda deshabilitado y atenuado.
  Si `Inh.=off`, el valor vuelve a ser editable.
- Si una propiedad no está soportada por el sistema operativo de la conexión, aparece atenuada y no se puede editar.
  Ejemplos: `sharesmb` en macOS, `jailed` fuera de FreeBSD, `zoned`/`nbmand` fuera de Linux.
- Los pools no importables también aparecen como nodo raíz para permitir `Importar`.
- Logs: panel único `Log combinado` (incluye salida SSH/PSRP con prefijo de conexión).
- En la tabla de conexiones hay un botón flotante `Conectividad`.
  Abre una matriz donde cada fila es la conexión origen y cada columna la conexión destino.
- Un `✓` en una celda indica que desde la máquina de la fila se puede abrir conexión directa hacia la máquina de la columna usando las credenciales definidas.
- Si falta ese `✓`, ZFSMgr no podrá hacer transferencia remota directa entre ese origen y ese destino.
  En ese caso, la transferencia tendrá que pasar por la máquina local donde se ejecuta ZFSMgr.
  Eso implica doble salto, más tráfico local y un coste mayor en tiempo y recursos.
- La zona inferior usa pestañas:
  - `Cambios pendientes` como primera pestaña visible por defecto
  - `Log combinado`, que incluye la caja `Aplicación` para el log textual
- El encabezado de cada treeview tiene menú contextual para ajustar una columna o todas al contenido y para cambiar `Columnas de propiedades`.
- El scroll vertical de los treeviews es suave, por píxel.
- El menú contextual del árbol permite además mostrar u ocultar `Información del pool`, `Propiedades` y `Permisos` inline.
- El botón `Aplicar cambios` solo se activa si hay comandos pendientes reales.
- `Cambios pendientes` muestra una descripción legible por línea, con prefijo `conexión::pool`, y no el comando real.
- Los cambios pendientes se conservan en orden de ejecución.
- `Mover` no ejecuta nada al instante: añade un `zfs rename` pendiente para mover el dataset de `Origen` dentro del dataset `Destino`.
  Solo se habilita si ambos son datasets del mismo pool y la misma conexión.
- `Renombrar` en el menú contextual de dataset/snapshot/zvol también trabaja en modo diferido y añade un `zfs rename` a `Cambios pendientes`.
- Al hacer clic en una línea de `Cambios pendientes`, ZFSMgr intenta llevar el foco al dataset y sección afectados.
  Si el pool está visible en ambos árboles, se prioriza `Origen`.
- `Copiar` y `Nivelar`, cuando usan dos conexiones SSH remotas distintas, intentan transferir directamente de `Origen` a `Destino`.
  El tráfico de datos no pasa por la máquina donde se ejecuta ZFSMgr; esa máquina solo mantiene la sesión de control y recibe el progreso.
- `Diff` muestra sus resultados en una ventana de árbol con `Añadido`, `Borrado`, `Modificado` y `Renombrado`.

Creación de pools:

<img src="help-img/crearpool.png" alt="Crear pool" width="50%">

- `Crear pool` abre un diálogo con splitter horizontal:
  - a la izquierda: `Parámetros del pool` y `Constructor de VDEV`
  - a la derecha: `Block devices disponibles`
- `altroot` arranca vacío por defecto.
  Si está vacío, no se añade `-R` al comando `zpool create`.
- `Block devices disponibles` muestra un árbol de dispositivos y particiones con columnas de tamaño, tipo de partición, si está montada y si ya pertenece a un pool.
- En macOS también aparecen discos físicos sin particiones.
- En macOS, los discos APFS internos/sintetizados del sistema no son seleccionables.
- La columna `Montada` permite desmontar desde el propio diálogo (`diskutil unmount`/`umount`).
- Un dispositivo ya usado en la estructura del pool queda marcado como no disponible y no puede reutilizarse en otro nodo.
- `Constructor de VDEV` ya no usa texto libre:
  - el nodo raíz es `Pool`
  - el menú contextual crea nodos válidos
  - los block devices se arrastran al árbol
  - los nodos del propio árbol también pueden reordenarse por arrastre
- La estructura del árbol sigue una gramática restringida compatible con OpenZFS:
  - en la raíz puede haber devices directos (stripe implícito), `mirror`, `raidz*` y clases top-level (`log`, `cache`, `special`, `dedup`, `spare`)
  - dentro de un vdev normal solo puede haber devices
  - `log` solo admite `mirror` como subgrupo
  - `special` y `dedup` admiten devices directos o subgrupos `mirror`/`raidz*`
  - `cache` y `spare` admiten solo devices directos
- Debe existir al menos un grupo de datos en la raíz del pool, ya sea como devices directos o como `mirror`/`raidz*`.
- Debajo del splitter aparece una previsualización del comando `zpool create` a todo el ancho.
- Esa previsualización se actualiza con:
  - cambios en la estructura del árbol
  - cambios en `Parámetros del pool`
  - argumentos extra
- Si la estructura no es válida, la previsualización se pinta en rojo.
- Si falla `Crear pool`, el diálogo no se cierra; se mantiene abierto para corregir datos y reintentar.

Creación y montaje de datasets:

<img src="help-img/creardataset.png" alt="Crear dataset" width="50%">

- `Crear dataset` se ejecuta desde el árbol de contenido.
- Si el dataset usa:
  - `encryption=on` o `aes-*`
  - `keyformat=passphrase`
  - `keylocation=prompt`
  el diálogo pide `Passphrase cifrado` y `Repetir passphrase`.
- Esa passphrase se envía por entrada estándar al crear el dataset; no se añade a la línea de comando mostrada en confirmaciones o logs.
- Si falla `Crear dataset`, el diálogo permanece abierto con los datos introducidos para poder corregirlos y reintentar.
- Al montar un dataset cifrado con `keylocation=prompt`, ZFSMgr pide antes la passphrase, ejecuta `zfs load-key` y luego `zfs mount`.

Comportamiento de navegación:

- Cambiar de conexión/pool reutiliza caché de datos.
- No se refresca automáticamente por solo navegar.
- Se refresca al ejecutar acciones que modifican estado/datos o con refresco explícito.
- Antes de cada acción se guarda/restaura el estado visual de ambos árboles.
  Cuando aplica, eso incluye selección, expansión de nodos y posición de scroll.
- Si una modificación afecta a un pool mostrado en ambos árboles, ambos treeviews se reconstruyen y restauran su estado.
- Al pinchar un nodo `Propiedades` vacío, sus hijos se cargan y el nodo queda desplegado.
- Si cambia `Columnas de propiedades`, un nodo `Propiedades` ya desplegado conserva su expansión.
- Si ningún check `Origen`/`Destino` está activo para un árbol, ese árbol queda vacío pero conserva cabeceras coherentes.
- `O` y `D` se persisten entre ejecuciones.
- El menú `Seleccionar snapshot` del árbol solo se habilita si el dataset tiene snapshots.

Revise `Navegación y estados` y `Propiedades inline y columnas` para detalles de funcionamiento.
