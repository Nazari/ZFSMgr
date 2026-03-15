# Acción Diff

`Diff` compara:

- en `Origen`: un snapshot
- en `Destino`: el dataset padre actual de ese snapshot, o bien otro snapshot del mismo dataset

Restricciones:

- Origen y Destino deben estar en la misma conexión
- Origen y Destino deben estar en el mismo pool
- ambos deben referirse al mismo dataset base

ZFSMgr ejecuta:

```sh
zfs diff <origen> <destino>
```

Ejemplos válidos:

- `pool/ds@s1` frente a `pool/ds`
- `pool/ds@s1` frente a `pool/ds@s2`

Resultado:

- se abre una ventana con cuatro nodos raíz:
  - `Añadido`
  - `Borrado`
  - `Modificado`
  - `Renombrado`
- debajo se muestran jerárquicamente los archivos y directorios detectados por `zfs diff`
- los renombrados muestran la ruta nueva y conservan la ruta anterior en el tooltip

Progreso y timeout:

- mientras `zfs diff` va emitiendo líneas, estas se registran en `Progreso`
- el timeout es por inactividad, no por duración total
- si no hay salida, ZFSMgr informa en `Progreso` cada 10 segundos del tiempo restante antes del timeout

La ventana es solo informativa y se cierra con `Aceptar`.
