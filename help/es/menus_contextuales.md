# Menús contextuales

La GUI usa menús contextuales por clic derecho en dos zonas.

## Menú contextual en `Conexiones`

Sobre la conexión seleccionada:

- `Refrescar`
- `Editar`
- `Borrar`

Notas:

- `Editar` y `Borrar` se deshabilitan para `Local` y conexiones redirigidas a `Local`.
- Durante acciones en curso, `Refrescar` queda bloqueado.

## Menú contextual en `Contenido <pool>`

Sobre el dataset/snapshot seleccionado:

- `Rollback`
- `Crear`
- `Borrar`
- `Origen`
- `Destino`
- `Desglosar`
- `Ensamblar`
- `Desde Dir`
- `Hacia Dir`

## Reglas

- Las acciones destructivas piden confirmación.
- El estado habilitado/deshabilitado sigue la misma lógica de validación que el resto de acciones.
- Durante una acción en curso se bloquean opciones no seguras.
