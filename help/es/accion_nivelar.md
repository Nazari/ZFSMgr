# Accion: Nivelar

Objetivo: alinear estado entre origen y destino usando snapshot/dataset.

Condiciones:

- Origen: dataset o snapshot.
- Destino: dataset.

Comportamiento:

- Calcula comando segun seleccion y estado remoto.
- Ejecuta transferencia con validaciones previas.
- Registra subcomandos en nivel INFO.

