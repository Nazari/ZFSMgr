# Menus contextuales

ZFSMgr usa menus contextuales (boton derecho) en varias vistas.

## Donde aparecen

- Arboles de Datasets (Origen, Destino y Avanzado).
- Listado de conexiones.

## Datasets (arboles)

Sobre un dataset/snapshot puede usar:

- Montar
- Montar con todos los hijos
- Desmontar
- Crear hijo
- Modificar
- Borrar
- Borrar todos los snapshots
- Rollback (si hay snapshot seleccionado)

Notas:

- Algunas opciones se desactivan segun estado montado/desmontado.
- Si hay una accion en curso, el menu puede estar bloqueado.

## Conexiones (listado)

Sobre una conexion puede usar:

- Refrescar
- Editar
- Borrar

## Recomendaciones

- Seleccione primero el nodo objetivo y luego abra el menu.
- Revise siempre la ventana de comprobacion antes de aceptar una accion destructiva.

