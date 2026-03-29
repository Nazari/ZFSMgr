# Propiedades inline y columnas

ZFSMgr muestra propiedades de dataset y pool directamente dentro del árbol unificado.

## Dónde aparecen

- En datasets y snapshots, bajo `Dataset properties`.
- En datasets no snapshot, también puede aparecer `Permisos`.
- En el nodo dual pool/dataset raíz, puede aparecer `Pool Information`.
- En datasets filesystem, puede aparecer `Programar snapshots`.
- En pools con datasets GSA activos, puede aparecer `Datasets programados`.

## Visualización

- El número de columnas visibles se ajusta desde el menú contextual del encabezado del árbol.
- El rango actual de `Columnas de propiedades` es:
  - `4, 6, 8, 10, 12, 14, 16`
- Los anchos de columna se guardan y se restauran.
- El scroll vertical del árbol es suave.

## Gestión de propiedades visibles

Con clic derecho sobre:

- `Dataset properties`
- `Pool Information`

puede abrir `Gestionar visualización de propiedades`.

Ese diálogo permite:

- elegir propiedades visibles
- reordenarlas por arrastrar y soltar
- crear grupos
- renombrarlos
- borrar grupos

Los grupos son independientes por:

- pool
- dataset
- snapshot

## Visibilidad de nodos inline

Desde el menú contextual del árbol puede activar o desactivar:

- `Mostrar propiedades en línea`
- `Mostrar Permisos en línea`
- `Mostrar Información del Pool`
- `Mostrar Datasets programados`

Efectos:

- si desactiva `Mostrar propiedades en línea`, desaparece `Dataset properties`
- si desactiva `Mostrar Permisos en línea`, desaparece `Permisos`
- si desactiva `Mostrar Información del Pool`, desaparece `Pool Information`
- si desactiva `Mostrar Datasets programados`, desaparece `Datasets programados`

## Edición inline

- Las propiedades editables se modifican directamente en el árbol.
- Las propiedades heredables muestran `Inh.` cuando aplica.
- Los permisos ZFS también se editan inline, pero en modo borrador.
- Las propiedades `org.fc16.gsa:*` no usan control de herencia visual.
- Al pulsar una línea en `Pending changes`, ZFSMgr intenta enfocar el objeto y la sección correspondiente.
