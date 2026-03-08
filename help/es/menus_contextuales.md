# Menús contextuales

La GUI combina botones visibles y menú contextual por clic derecho.

## Flujo actual

- En `Conexiones` y `Nuevo` se usan botones.
- En `Acciones` (panel izquierdo) se usan botones para transferencias.
- En `Contenido <pool>`, las acciones del dataset/snapshot se lanzan con clic derecho sobre el árbol.
- En `Propiedades del dataset` se aplica con `Aplicar cambios`.

## Menú contextual en `Contenido <pool>`

Sobre el dataset/snapshot seleccionado, el menú contextual ofrece:
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

- Durante una acción en curso, las opciones no seguras quedan bloqueadas.
- Las acciones destructivas siempre piden confirmación.
- El estado habilitado/deshabilitado de cada opción sigue la misma lógica que en botones.

## Recomendaciones

- Revise siempre la ventana de comprobación antes de ejecutar.
- Use `Reset` en acciones para limpiar origen/destino cuando sea necesario.
