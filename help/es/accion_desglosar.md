# Accion: Desglosar

Objetivo: convertir directorios del dataset en subdatasets.

Condiciones:

- Dataset seleccionado en el árbol del pool.
- El dataset debe estar **montado**: si no lo está, no se listará ningún directorio candidato.
- En conexiones Unix requiere el agente `zfsmgr-agent` instalado: no hay alternativa por shell.

## Elegir qué convertir, y con qué nombre

La pantalla de selección tiene dos columnas: el directorio y **el dataset en el que se va a convertir**, que se puede editar.

**No hace falta convertir también los directorios de encima.** `zfs create` solo exige que exista el dataset *padre por nombre*; le da igual la ruta del punto de montaje. Por eso se puede convertir `a/b/c` dejando `a` y `b` como directorios normales.

El nombre propuesto refleja esa decisión: los tramos que también se convierten se separan con `/`, y los que no, con `:`.

```
marcando solo  a/b/c   ->  <dataset>/a:b:c
marcando a, b y c      ->  <dataset>/a/b/c
```

La columna se recalcula al marcar y desmarcar, salvo en las filas cuyo nombre haya editado usted. ZFS solo admite `a-z A-Z 0-9 _ . : -` y espacio; `:` es de los pocos legales que casi no aparece en nombres de directorio, por eso se usa como marca de «este tramo no es un dataset». Un nombre no válido, repetido o ya ocupado sale en rojo con el motivo y bloquea el botón Aceptar.

Se ocultan los directorios que ya corresponden a subdatasets con su nombre canónico. Los que solo se muestran para situar a otros salen deshabilitados. Los nombres no válidos como dataset (con `@`, `#`, `,`, `..` o caracteres de control) se marcan como no seleccionables.

## Qué hace

- Muestra el comando, pide confirmación y se ejecuta en el acto, bloqueando la interfaz mientras dura.
- Cada byte se mueve **una sola vez**: primero se copia cada directorio a un dataset con nombre provisional y plano, excluyendo los subdirectorios que también van a convertirse; solo cuando **todas** las copias están verificadas se borran los originales y se renombran los datasets a su nombre definitivo.
- Un directorio que **ya es** el punto de montaje de un dataset con otro nombre —lo que deja un Ensamblar que conservó los subdatasets— no se copia: solo se le cambia el nombre.
- Si dentro de un directorio a convertir hay subdatasets montados, se desmontan antes de borrar nada y se vuelven a montar al terminar.
- La caja de *Progreso* publica una línea cada dos segundos con el elemento en curso, lo copiado y la velocidad:

```
[BREAKDOWN] 8 de 13: Disks/Bootables/Tools — 643.2 MiB copiados a 64.1 MiB/s
```

## Advertencias importantes

- El espacio intermedio se toma **del propio pool**, donde los datos tienen que acabar; no del `/tmp` del sistema. El paso final es un cambio de nombre, no una segunda copia.
- Antes de borrar el directorio origen se **verifica que la copia esté completa** (pasada en seco de `rsync` contando ficheros pendientes, con reintentos). Si tras varios intentos falta algo, la acción se detiene y **no borra nada**.
- El borrado del original **no cruza puntos de montaje**: si encontrara algo montado que no se hubiera desmontado antes, se detiene en vez de borrar su contenido.
- Si falla el renombrado final, los datos están íntegros en los datasets provisionales y el mensaje dice cuáles son y a qué nombre les corresponde ir. **No los destruya.**
- La copia preserva **ACLs y atributos extendidos** cuando el `rsync` del sistema los soporta (se detecta automáticamente).
