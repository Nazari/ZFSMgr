# Menús contextuales

La GUI usa menús contextuales por clic derecho en tres zonas.

## Menú contextual en `Conexiones`

Sobre una fila de conexión:

- `Refrescar`
- `Editar`
- `Borrar`
- `Refrescar todas las conexiones`
- `Nueva conexión`
- `Nuevo pool`

Notas:

- `Editar` y `Borrar` se deshabilitan para `Local` y conexiones redirigidas a `Local`.
- Durante acciones en curso, `Refrescar` queda bloqueado.
- Si el clic derecho se hace en zona vacía, solo aparecen opciones globales (`Refrescar todas`, `Nueva conexión`, `Nuevo pool`).

## Menú contextual en los árboles de detalle

Sobre el nodo raíz `Pool`:

- `Actualizar`
- `Importar`
- `Exportar`
- `Historial`
- `Sync`
- `Scrub`
- `Trim`
- `Initialize`
- `Destroy`
- `Mostrar Información del pool`

Sobre dataset/snapshot seleccionado:

- `Gestionar visualización de propiedades`
- `Mostrar propiedades en línea`
- `Mostrar Permisos en línea`
- `Crear dataset/snapshot/vol`
- `Renombrar`
- `Borrar Dataset <nombre>`, `Borrar Snapshot <dataset@snapshot>` o `Borrar ZVol <nombre>` según el objetivo real
- `Encriptación`
  - `Load key`
  - `Unload key`
  - `Change key`
- `Seleccionar snapshot`
- `Rollback`
- `Nuevo Hold`
- `Release <hold>`
- `Desglosar`
- `Ensamblar`
- `Desde Dir`
- `Hacia Dir`

Sobre el nodo `Permisos` de un dataset:

- `Refrescar permisos`
- `Nueva delegación`
- `Nuevo set de permisos`

Sobre una delegación:

- `Editar delegación`
- `Eliminar delegación`

Sobre un set de permisos:

- `Renombrar conjunto de permisos`
- `Eliminar set`

Sobre el encabezado de cualquiera de los treeviews:

- `Ajustar tamaño de esta columna`
- `Ajustar tamaño de todas las columnas`
- `Columnas de propiedades`
  - permite elegir entre `5` y `10` columnas

## Reglas

- Las acciones destructivas piden confirmación.
- El estado habilitado/deshabilitado sigue la misma lógica de validación que el resto de acciones.
- Durante una acción en curso se bloquean opciones no seguras.
- `Gestionar visualización de propiedades` está disponible tanto en propiedades de dataset como en `Información del pool`.
- Ese diálogo permite elegir qué propiedades se muestran, reordenarlas por arrastrar y soltar y crear o borrar grupos de visualización.
- Dataset, pool y snapshot usan grupos de visualización independientes.
- En snapshots, la propiedad `snapshot` queda fija en el grupo principal.
- `Mostrar propiedades en línea`, `Mostrar Permisos en línea` y `Mostrar Información del pool` cambian la estructura visible del árbol y se guardan en configuración.
- El árbol ya no usa nodos intermedios `Contenido` ni `Subdatasets`.
- En `Permisos`, la edición de checks es diferida.
  Los cambios se acumulan y se aplican con el botón `Aplicar cambios`.
- La pestaña `Cambios pendientes` del panel inferior lista esos cambios en el mismo orden en que se ejecutarían.
- Al hacer clic en una línea de `Cambios pendientes`, ZFSMgr intenta enfocar el dataset y la sección afectada (`Propiedades` o `Permisos`).
- `Renombrar` sobre dataset, snapshot o zvol no ejecuta el cambio al momento.
  Abre un diálogo para pedir el nuevo nombre y añade un `zfs rename` pendiente.
- `Nuevo Hold` solo aplica a snapshots.
- `Release <hold>` solo aparece sobre un hold o su propiedad `TimeStamp`.
- `Encriptación` solo se habilita en datasets que son raíz de encriptación.
- Si `keylocation=prompt`, `Load key` pide la clave y `Change key` abre una ventana para introducir la nueva clave dos veces.
- El menú del encabezado ajusta el ancho como un doble clic sobre el separador de columnas.
- El mismo menú del encabezado concentra también la configuración de `Columnas de propiedades`.
- La caja `Acciones` incluye también `Mover`, que añade un `zfs rename` pendiente para mover el dataset `Origen` dentro del dataset `Destino` en el mismo pool.
- La caja `Acciones` incluye también `Diff`, que compara un snapshot de Origen con su dataset padre actual o con otro snapshot del mismo dataset.
