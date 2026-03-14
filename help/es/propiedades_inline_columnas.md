# Propiedades inline y columnas

ZFSMgr puede mostrar propiedades de dataset y pool directamente dentro de los árboles de detalle.

## Dónde aparecen

- En un dataset, las propiedades se muestran inline bajo el nodo `Propiedades`.
- En un pool, las propiedades se muestran inline bajo el nodo `Información`.

## Visualización

- El número de columnas visibles depende de `Menú > Ventana > Columnas de propiedades`.
- Cada árbol usa sus propios anchos de columna.
- El ajuste de anchura se hace por contenido visible, similar a un doble clic en el separador de columnas.

## Gestión de propiedades visibles

Con clic derecho sobre:

- `Propiedades` de un dataset, o
- `Información` de un pool

puede abrir `Gestionar visualización de propiedades`.

Ese diálogo permite:

- marcar qué propiedades se muestran,
- reordenarlas por arrastrar y soltar,
- mantener la misma cantidad de columnas que el árbol,
- guardar la selección y el orden en configuración.

Las propiedades no marcadas no se muestran en el árbol, aunque sigan estando disponibles en la lista de gestión.

## Edición inline

- Las propiedades editables pueden modificarse directamente en el árbol.
- Si una propiedad es heredable, aparece la marca `Inh.` en el nombre.
- En ese caso la celda de valor puede incluir un control adicional `off/on` para aplicar `zfs inherit`.
- El estado inicial de ese control refleja si la propiedad ya está heredada o no.
