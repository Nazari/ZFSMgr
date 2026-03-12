# Accion: Nivelar

Objetivo: alinear estado entre origen y destino usando snapshot/dataset.

Condiciones:

- Origen: dataset o snapshot.
- Destino: dataset.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Comportamiento:

- Calcula comando segun seleccion y estado remoto.
- Ejecuta transferencia con validaciones previas.
- Registra subcomandos en nivel INFO.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.
