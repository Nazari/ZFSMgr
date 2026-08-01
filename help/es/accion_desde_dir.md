# Accion: Desde Dir

Objetivo: crear un dataset hijo a partir de uno o varios directorios existentes.

Condiciones:

- Dataset seleccionado en el árbol del pool.
- La seleccion debe ser dataset (no snapshot).
- En conexiones Unix requiere el agente `zfsmgr-agent` instalado: no hay alternativa por shell.

Comportamiento:

- Abre una ventana para definir el nuevo dataset (propiedades, creación de padres con `-p` y cifrado opcional) y para marcar los directorios origen.
- Puede marcar **varios directorios, incluso de conexiones distintas**. Si marca un directorio que ya contiene a otro marcado, se descarta el descendiente. La jerarquía relativa de cada directorio se reproduce dentro del dataset destino.
- Crea el dataset, fuerza `canmount=on` y lo monta en su punto de montaje definitivo.
- Copia el contenido con `tar --acls --xattrs` en conexiones Unix, preservando ACLs y atributos extendidos. En conexiones Windows se usa `tar` sin ACLs ni xattrs.
- Si la copia termina correctamente y marcó la casilla correspondiente, borra los directorios origen.
- La acción se añade a `Cambios pendientes` y solo se ejecuta al aplicar los cambios.
- Registra comandos y resultado en el log combinado.

Advertencias importantes:

- **El dataset queda montado al terminar**, también si decide no borrar los directorios origen.
- No hay verificación posterior de la copia: el borrado del origen depende únicamente del código de salida de la copia.
