# Menus contextuales

La GUI actual prioriza botones de accion visibles en pantalla.

## Flujo actual

- En `Conexiones`, `Nuevo` y `Acciones` se usan botones.
- En `Contenido <pool>` se usan botones bajo el arbol de datasets.
- En `Propiedades del dataset` se aplica con `Aplicar cambios`.

## Menus contextuales

Si aparece algun menu contextual puntual en su build, aplica la misma regla:
- durante una accion en curso, opciones no seguras quedan bloqueadas;
- las acciones destructivas siempre piden confirmacion.

## Recomendaciones

- Revise siempre la ventana de comprobacion antes de ejecutar.
- Use `Reset` en acciones para limpiar origen/destino cuando sea necesario.
