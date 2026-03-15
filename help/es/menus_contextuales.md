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
- `Scrub`
- `Destroy`

Sobre dataset/snapshot seleccionado:

- `Gestionar visualización de propiedades`
- `Seleccionar snapshot`
- `Rollback`
- `Crear dataset/snapshot/vol`
- `Nuevo Hold`
- `Release <hold>`
- `Borrar Dataset <nombre>`, `Borrar Snapshot <dataset@snapshot>` o `Borrar ZVol <nombre>` según el objetivo real
- `Desglosar`
- `Ensamblar`
- `Desde Dir`
- `Hacia Dir`

Sobre el encabezado de cualquiera de los treeviews:

- `Ajustar tamaño de esta columna`
- `Ajustar tamaño de todas las columnas`

## Reglas

- Las acciones destructivas piden confirmación.
- El estado habilitado/deshabilitado sigue la misma lógica de validación que el resto de acciones.
- Durante una acción en curso se bloquean opciones no seguras.
- `Gestionar visualización de propiedades` está disponible tanto en propiedades de dataset como en `Información` del pool.
- Ese diálogo permite elegir qué propiedades se muestran, reordenarlas por arrastrar y soltar y crear o borrar grupos de visualización.
- Dataset, pool y snapshot usan grupos de visualización independientes.
- En snapshots, la propiedad `snapshot` queda fija en el grupo principal.
- `Nuevo Hold` solo aplica a snapshots.
- `Release <hold>` solo aparece sobre un hold o su propiedad `TimeStamp`.
- El menú del encabezado ajusta el ancho como un doble clic sobre el separador de columnas.
