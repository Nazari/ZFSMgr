# El modo interactivo del CLI

`zfsmgr-cli` sin ninguna orden se comporta como un intérprete: hay una **posición**, que es
una URL `zfsm://`, y todas las órdenes actúan sobre ella salvo que se diga otra cosa.

```
zfsm://local> cd fc16/work
zfsm://local/fc16/work> ls
zfsm://local/fc16/work> snapshot @antes
zfsm://local/fc16/work> ls --on /unibody/sback
```

## Por qué la posición es una URL y no «una conexión y un dataset»

Porque así lo que se aprende navegando sirve para escribir guiones: cualquier orden del
historial se puede copiar, ponerle `--on <url>` y ejecutarla suelta. Si la posición fuera
un estado interno con dos campos, la forma de nombrar las cosas dentro del intérprete y
fuera de él serían distintas, y habría que aprender las dos.

## El árbol, y las cuatro formas de moverse por él

El modelo es el de un sistema de ficheros, con la lista de conexiones como raíz:

```
/                          las conexiones
/OldLau                    una conexión
/OldLau/winpool            el pool, que ES un dataset
/OldLau/winpool/sa         un dataset
/OldLau/winpool/sa@ayer    una instantánea
```

`cd` acepta la URL entera, la ruta absoluta, la relativa, y `..` / `.` / `-`.

**La raíz no es una URL válida** —`zfsm://` sin conexión no nombra nada—, así que existe
solo dentro del intérprete, donde sí hace falta: es el sitio al que subir desde una
conexión. Se enseña como `zfsm:/`.

### Las dos reglas que quitan ambigüedad a una ruta relativa

Las dos salieron de escribir órdenes y ver qué hacía el intérprete con ellas:

1. **Si el primer tramo nombra una conexión, la ruta es absoluta.** `cd oldlau/winpool/sa`
   estando en `zfsm://local` va a OldLau. Sin esto habría que anteponer una barra que
   nadie recuerda al saltar de máquina.
2. **Si el primer tramo es el nombre del pool en el que estamos, es el nombre ZFS
   completo.** `destroy zfsmgrtest/clonado` desde `zfsm://local/zfsmgrtest/origen` apuntaba
   a `zfsmgrtest/origen/zfsmgrtest/clonado`: un nombre válido que no existe, o sea un error
   confuso donde uno había escrito exactamente lo que quería.

### `cd` comprueba que el destino existe

Como el de cualquier intérprete. Cuesta una ida y vuelta, pero por el túnel ya montado son
un par de milisegundos, y sin ello uno se queda apuntando a algo que no está: la orden
siguiente falla por un motivo que no parece tener nada que ver con el `cd` que la precedió.

## `--on` y `--from` son sinónimas

«from» es la palabra natural en las órdenes que además tienen un destino y «on» en las que
actúan sobre un solo sitio. Obligar a recordar cuál lleva cada una sería una regla que no
aporta nada.

**La excepción es `fromdir`**, donde sí significan cosas distintas: `--on` es el dataset de
destino y `--from` la máquina de la que sale el directorio.

## Los formatos, y para quién es cada uno

`format text|tsv|json` cambia la salida de los listados en caliente. Los tamaños salen
legibles —`464G`— en texto y en **bytes exactos** en tsv y json, porque un guion que compare
o sume no puede hacerlo con «1,2 G».

## Lo destructivo pregunta, y la pregunta dice qué

`destroy`, `rollback`, `breakdown`, `assemble`, `todir` y `fromdir` piden confirmación
nombrando lo que va a pasar y con qué alcance. «¿Seguro?» a secas es lo que hace que se
conteste que sí sin leer. `-y` en la línea de órdenes, o `yes on` dentro, lo desactivan.

## Lo que NO tiene respaldo por shell, a propósito

Las órdenes van por **RPC tipado**: argv, sin intérprete de por medio. Si el daemon no está
o no responde, se dice. En la interfaz existe un camino alternativo por shell por historia;
aquí se empieza limpio, porque ejecutar por otro camino una orden que el usuario creía
tipada es peor que fallar.

Las dos excepciones, y las dos son del dominio, no del diseño:

- **`ls #content`** lista con `ls -lA`. El agente no tiene verbo para leer un directorio.
- **`fromdir`** es una tubería `tar` entre dos máquinas. El verbo del agente lee el tar por
  la entrada estándar y **el canal RPC no tiene stdin**, cosa que el propio daemon deja
  dicha en su código.

## `fromdir` NO es la inversa de `todir`

Aunque el nombre lo sugiera. Es «Desde Dir»: crea el contenido de un dataset A PARTIR de un
directorio, que puede estar en otra máquina. La inversa de `todir` es `assemble`.

## Modo guion

Con la entrada redirigida no hay indicador ni confirmaciones interactivas, y **un fallo
detiene la sesión**: seguir ejecutando órdenes sobre un estado que no es el previsto es
cómo se destruye lo que no se quería tocar. El código de salida es el de la orden que falló.

```sh
printf 'cd fc16/work\nsnapshot @nocturna -r\n' | zfsmgr-cli --password-fd 3 -y 3< <(pass show zfsmgr)
```

## Cómo se probó

Contra el daemon real de la máquina, y sobre un **pool de pruebas montado en un fichero**
—creado y destruido para la ocasión, sin tocar nada existente—: navegación completa,
listados en los tres formatos, `#content` y `#properties`, `snapshot`, `get`/`set`,
`create`, `clone`, `destroy`, `rollback`, `breakdown`, `assemble`, `todir` y `fromdir`.

Se condujo por un pseudoterminal, porque el material TLS del daemon local vive en
`/etc/zfsmgr` con permisos de root y hace falta poder contestar a la petición de sudo.

**Dos fallos que encontró esa prueba y que no se habrían visto de otro modo:**

1. `assemble` recibía los hijos con nombre relativo. El agente los comprueba con
   `zfs list <hijo>`, así que no existían para él y la operación se saldaba con
   «ya absorbido» y **rc=0**: parecía haber funcionado sin hacer nada.
2. `-y` se parseaba y no se pasaba al intérprete, así que seguía preguntando.

## Lo que falta

- La resolución de rutas es lógica pura y está probada solo por las pruebas en vivo.
  Sacarla a la capa base la haría contrastable como el resto.
- El caso `fromdir --from <otra-máquina>` no se ha ejercitado: el material TLS de las
  conexiones remotas está cifrado con la contraseña maestra.
- No hay historial ni completado por tabulador: eso pide `readline`, y meter una
  dependencia por comodidad va en contra de lo que se ha hecho hasta ahora.
