# Accion: Clonar

Objetivo: clonar un snapshot sobre un dataset destino con `zfs clone`.

Requisitos para habilitar el boton:

- Origen debe ser un `snapshot`.
- Destino debe ser un `dataset` (sin snapshot seleccionado).
- Origen y destino deben estar en la misma conexion.
- Origen y destino deben pertenecer al mismo pool.

El dataset destino se propone como `<destino>/<nombre-hoja-del-origen>` (salvo que el destino ya termine con ese nombre) y puede editarse antes de aceptar.

Opciones disponibles en la ventana de Clonar:

- `-p` crear datasets padre si no existen.
- `-u` no montar automaticamente el clon.
- `-o propiedad=valor` (una por linea) para asignar propiedades al clon.

Comando base:

`zfs clone [-p] [-u] [-o propiedad=valor]... <origen@snapshot> <dataset_destino>`

Notas:

- Si no cumple condiciones, el boton aparece deshabilitado.
- La acción se añade a `Cambios pendientes` y solo se ejecuta al aplicar los cambios; al terminar se refresca la conexion destino y su contenido.
- Si alguna conexión usa OpenZFS por debajo de `2.3.3`, la acción se bloquea al ejecutarse, aunque el botón aparezca habilitado.
