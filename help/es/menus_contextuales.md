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

- `Mostrar propiedades en línea` (check)
- `Gestionar visualización de propiedades`
- `Editar`
- `Rollback`
- `Crear`
- `Borrar`
- `Desglosar`
- `Ensamblar`
- `Desde Dir`
- `Hacia Dir`

## Reglas

- Las acciones destructivas piden confirmación.
- El estado habilitado/deshabilitado sigue la misma lógica de validación que el resto de acciones.
- Durante una acción en curso se bloquean opciones no seguras.
- `Gestionar visualización de propiedades` está disponible tanto en propiedades de dataset como en `Información` del pool.
- Ese diálogo permite elegir qué propiedades se muestran y en qué orden aparecen en el árbol activo.
