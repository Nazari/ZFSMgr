# Accion: Clonar

> **Cómo se pide.** Marque el origen con el botón derecho (`Marcar como origen`) y luego abra el menú contextual **sobre el nodo destino**: el submenú `Con el origen …` ofrece esta acción. Ya no hay botón. Si sale en gris, el motivo está en su tooltip. Ver `Menús contextuales`.

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
- **Se ejecuta al pulsarla.** Antes se añadía a una lista de cambios pendientes y esperaba
  a que usted la aplicara; esa lista ya no existe.
- Si alguna conexión usa OpenZFS por debajo de `2.3.3`, la acción se bloquea al ejecutarse, aunque el botón aparezca habilitado.
