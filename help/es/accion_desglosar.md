# Accion: Desglosar

Objetivo: crear subdatasets a partir de directorios del dataset padre.

Condiciones:

- Dataset seleccionado en `Contenido <pool>`.
- Dataset y descendientes montados (segun reglas de seguridad).

Comportamiento:

- Muestra una pantalla para seleccionar directorios.
- Crea subdataset, copia datos y solo despues elimina origen.
- Registra progreso por directorio en log NORMAL.
