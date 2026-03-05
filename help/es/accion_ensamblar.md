# Accion: Ensamblar

Objetivo: convertir subdatasets en directorios dentro del dataset padre.

Condiciones:

- Dataset seleccionado en Avanzado.
- Dataset y descendientes montados (segun reglas de seguridad).

Comportamiento:

- Muestra una pantalla para seleccionar subdatasets.
- Copia contenido de cada subdataset al padre.
- Solo destruye subdataset si la copia termina correctamente.
- Registra progreso por subdataset en log NORMAL.

