# Accion: Ensamblar

Objetivo: absorber un subdataset en el dataset padre, dejando su contenido como directorio.

Condiciones:

- **Sí está disponible en conexiones Windows**, al contrario que `Desglosar`. Comprobado
  contra una máquina real: los datasets vuelven a ser directorios en su sitio.
- Dataset seleccionado en el árbol del pool.
- El dataset padre debe estar **montado**; si no lo está, la operación falla con `mountpoint=none`. Cada subdataset seleccionado lo monta el propio agente.
- En conexiones Unix requiere el agente `zfsmgr-agent` instalado: no hay alternativa por shell.

## Elegir qué absorber

La pantalla de selección tiene dos columnas: el directorio y **el dataset que es actualmente**. Aquí el nombre es un dato, no una propuesta, así que no se edita.

Los subdatasets se eligen **por separado**: absorber uno no obliga a absorber los que cuelgan de él.

## Qué hace

- Muestra el comando, pide confirmación y se ejecuta en el acto, bloqueando la interfaz mientras dura.
- El contenido se copia a una escala **dentro del propio pool**, y el paso final es un cambio de nombre en vez de una segunda copia: los datos no viajan dos veces.
- **Los subdatasets que cuelgan del absorbido se conservan.** No se destruyen: se reasignan al padre y siguen montados donde estaban, así que su contenido no se toca.
- Al reasignarlos, el nombre pasa a codificar **dónde quedan montados**: la ruta bajo el padre con `:` en lugar de `/`.

```
testpool/user/bin/Squirrel.app/Contents   (antes)
testpool/user/bin/Squirrel.app:Contents   (después, montado en el mismo sitio)
```

Así el nombre conserva la ruta y un Desglosar posterior puede reconstruirla. El nombre del dataset deja de reflejar la estructura de directorios, que es lo esperado: ZFS no exige que coincidan.

- La caja de *Progreso* publica una línea cada dos segundos con el elemento en curso, lo copiado y la velocidad.

## Advertencias importantes

- El espacio intermedio se toma **del propio pool**, no del `/tmp` del sistema. En la mayoría de las distribuciones de Linux `/tmp` está en memoria, y usarlo hacía imposible ensamblar un dataset grande.
- Antes de destruir el dataset de origen se **verifica que la copia esté completa**; si falta algo, la acción se detiene y el dataset **no se destruye**.
- Se destruye **solo** el dataset elegido, sin `-r`: sus descendientes ya se han reasignado antes.
- La copia preserva **ACLs y atributos extendidos** cuando el `rsync` del sistema los soporta (se detecta automáticamente).
