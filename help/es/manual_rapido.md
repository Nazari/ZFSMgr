# Manual rapido

ZFSMgr gestiona conexiones y acciones ZFS desde un árbol unificado.

## Vista general

![Ventana principal](qrc:/help/img/auto/main-window.png)

- Columna izquierda:
- `Selected datasets`: muestra el dataset marcado como `Origen` y el marcado como `Destino`.
- `Status and progress`: resume estado actual, carga y progreso.
- Columna derecha:
- un único árbol unificado con:
  - conexiones como nodos raíz
  - pools bajo cada conexión
  - datasets y snapshots bajo cada pool
- Debajo del árbol:
  - `Pending changes`
- Zona inferior:
  - logs

## Árbol unificado

- Las conexiones aparecen siempre como nodos raíz, incluso si están desconectadas.
- Si una conexión está desconectada:
  - la conexión sigue visible
  - sus pools desaparecen del árbol
- El color y el tooltip de cada conexión mantienen la semántica de estado que antes tenía la tabla de conexiones.
- Si una conexión necesita atención GSA, su nombre aparece con `(*)`.
- Los pools ya no se muestran como `Conexion::Pool`; el texto visible del pool es solo el nombre del pool.
- El nodo raíz del pool está fusionado con el dataset raíz del pool:
  - mantiene el icono y el tooltip de pool
  - actúa también como dataset raíz
  - sus hijos cuelgan directamente de él
- En pools importados puede aparecer:
  - `Pool Information`
  - `Datasets programados`

## Nodos inline

- En datasets y snapshots puede aparecer `Dataset properties`.
- En datasets no snapshot también puede aparecer `Permisos`.
- En datasets filesystem puede aparecer `Programar snapshots`.

- Vista del nodo `Programar snapshots`:

![Nodo Programar snapshots](qrc:/help/img/auto/schedule-snapshots-node.png)

- Las propiedades inline pueden editarse directamente en el árbol.
- Si una propiedad admite herencia, aparece `Inh.` y el borrador se acumula sin ejecutar inmediatamente.
- `Permisos` también trabaja en modo borrador.
- `Programar snapshots` usa propiedades `org.fc16.gsa:*`.

## Selección de origen y destino

- Ya no existe selección `Origen/Destino` por checks en una tabla.
- Para elegirlos:
  - clic derecho sobre un dataset
  - `Seleccionar como origen`
  - `Seleccionar como destino`
- La caja `Selected datasets` refleja esa selección lógica.
- La selección visual actual del árbol y las selecciones lógicas `Origen/Destino` son independientes.

## Menús contextuales

- Sobre una conexión:
  - aparece el antiguo menú contextual de conexiones
- Sobre el nodo raíz fusionado del pool:
  - aparece primero un submenú `Pool`
  - luego el resto de acciones de dataset
- El submenú `Pool` concentra acciones de pool:
  - `Actualizar`
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
    - `Destroy`
  - `Mostrar Información del Pool`
  - `Mostrar Datasets programados`
- En datasets/snapshots sigue habiendo acciones como:
  - `Crear dataset/snapshot/vol`
  - `Renombrar`
  - `Borrar`
  - `Encriptación`
  - `Seleccionar snapshot`
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
  - `Rename`
  - `Move`
  - borrado diferido de datasets/snapshots

## Conectividad y logs

- `Comprobar conectividad` ya no es un botón flotante.
- Ahora está en el menú principal de la aplicación.
- `Combined log` sigue mostrando salida de aplicación y de conexiones.

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

## Navegación

- El árbol recuerda expansión, selección y snapshots seleccionados.
- Cambiar columnas de propiedades conserva la apertura de nodos visibles.
- Al pulsar un nodo vacío de `Dataset properties`, sus hijos se materializan y el nodo queda abierto.
