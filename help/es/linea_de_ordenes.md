# Línea de órdenes (`zfsmgr-cli`)

ZFSMgr trae una herramienta de terminal que hace lo mismo que la ventana, contra las
**mismas conexiones**: lee `config.json` y `trust-store.json` del mismo sitio, habla por
el mismo túnel cifrado y ejecuta los mismos verbos del agente. No es un programa aparte
con su propia configuración.

En Windows el instalador la deja en el `PATH` —si deja marcada esa casilla—, así que se
llama por su nombre desde `cmd` o PowerShell. En Linux y macOS **se instala junto a la
aplicación**, en `bin`, así que también se llama por su nombre. Antes no era así y no salía
en ningún paquete de Unix.

Hay además un **servidor web**, `zfsmgr-web`: enseña lo mismo en un navegador, sin
JavaScript, y se llega a él por un túnel SSH. Los tres —ventana, intérprete y servidor—
hablan con el mismo agente y comparten las mismas reglas.

## Dos formas de usarla

**Con una orden**, para guiones:

```sh
zfsmgr-cli connections list
zfsmgr-cli --format json connections list | jq '.connections[] | select(.tls == false)'
```

**Sin ninguna orden**, y entonces se comporta como un intérprete: hay una posición
—una URL `zfsm://`— y todo lo que se teclea actúa sobre ella.

```text
zfsm://local> cd oldlau/winpool
zfsm://oldlau/winpool> ls
zfsm://oldlau/winpool> cd sa@ayer
zfsm://oldlau/winpool/sa@ayer> ls #content
```

`help` enumera las órdenes y `help <orden>` explica una. El tabulador completa órdenes
y URLs, y las flechas recorren el historial.

## La posición es una URL

La misma que usa la aplicación: `zfsm://conexión/pool/dataset@instantánea#sección`. Las
secciones son `#content[/ruta]` (los ficheros de dentro), `#properties[/prop]` y
`#permissions`, y **van en inglés** como el resto de la URL.

Eso tiene una consecuencia práctica: **cualquier orden del historial se puede copiar y
ejecutar suelta**, añadiéndole `--on <url>` (o `--from`, que es lo mismo). Las órdenes
que necesitan origen y destino toman como origen la posición actual si no se dice otra
cosa.

Dos reglas quitan la ambigüedad a una ruta relativa:

- Si el primer tramo nombra una **conexión**, la ruta es absoluta. Es lo que se escribe
  al saltar de una máquina a otra: `cd unibody` desde `zfsm://local`.
- Si el primer tramo es el **pool en el que ya está**, se toma como el nombre ZFS
  completo y no como un hijo. Estando en `tank/origen`, `destroy tank/clon` apunta a
  `tank/clon`, no a `tank/origen/tank/clon`.

Para bajar a un hijo que se llame igual que una conexión, `./nombre`.

**Varias rutas de una vez.** Dentro de `#content` se admite la notación de llaves:
`zfsm://conn1/pool1/ds1#content/{fotos,docs}` nombra los dos subárboles. Hoy la usa
`rsync`, que copia cada uno al mismo destino. No se anidan ni se combinan con nada más.

## Abreviar las órdenes

Basta con escribir las primeras letras mientras no haya dos órdenes que empiecen igual.
`pw` es `pwd`, `inf` es `info`, `ro` es `rollback`. Cuatro se quedan en una sola letra:
`b`, `g`, `q` e `y`.

**No hace falta memorizar cuántas letras.** Escriba las que le parezcan y, si no bastan, el
intérprete enumera entre cuáles está dudando:

```
> j
«j» es ambigua: job, jobs

> cl
«cl» es ambigua: clear, clone, cls
```

Eso es distinto de una orden que no existe, que sigue diciendo «orden desconocida». La
misma lista es la que ofrece el TABULADOR, así que completar y abreviar nunca discrepan.

**Lo exacto gana siempre.** `job` es una orden y `jobs` es otra: escribir `job` ejecuta
`job`, aunque `jobs` también empiece por ahí. Sin esa regla no habría forma de escribir la
corta. Pasa lo mismo con `hold` y `holds`, `schedule` y `schedules`, y `export` y
`export-trust`.

La abreviatura vale **solo para la orden**, que es la primera palabra. Lo que va detrás
—destinos, opciones, valores— se escribe entero.

## Los tres formatos de salida

- `text` (por omisión) es para leer: columnas alineadas, cabeceras traducidas, tamaños
  legibles y los booleanos como `sí`/`no`.
- `tsv` es para guiones: sin cabecera, separado por tabuladores, columnas fijas **en
  inglés** y `-` donde no hay valor, igual que `zfs list -H`.
- `json` es para programas: los números salen como números, los booleanos como
  booleanos y lo que no aplica como `null`.

**En `tsv` y en `json` los nombres de campo van siempre en inglés y no cambian con el
idioma.** Es a propósito: un guion no debería dejar de funcionar porque alguien cambie
el idioma de la interfaz.

## El idioma

`--lang es|en|zh`. Sin esa opción usa el que tenga configurado la interfaz gráfica
(`app.language` de `config.json`), para que las dos herramientas hablen igual.

Lo que se traduce son los mensajes, la ayuda y las cabeceras en formato `text`. Lo que
**no** se traduce, y no es un olvido: los verbos que se teclean, los nombres de campo
de `tsv` y `json`, y los literales de las URL.

## Las contraseñas

Nunca por argumento ni por variable de entorno: las dos cosas quedan visibles en `ps`
para cualquier usuario de la máquina. Solo por terminal o por descriptor de fichero, lo
que permite usar cualquier gestor de secretos:

```sh
zfsmgr-cli --password-fd 3 connections list  3< <(pass show zfsmgr)
```

Con `--no-secrets` no se descifra nada y no se pide la contraseña maestra: los campos
cifrados salen como `<cifrado>`. Sirve para inventariar sin tener el secreto delante.
Con esa opción **no se escribe la configuración**: guardar un perfil cuyos campos
cifrados llegaron vacíos los dejaría vacíos en el fichero, y eso borra la contraseña
guardada.

## Acciones destructivas

Se pide confirmación antes de cada una, enumerando lo que se lleva por delante. `-y`
las da por confirmadas; sin terminal y sin `-y`, la orden se niega a seguir en vez de
suponer que sí.

## Cuando algo va mal

`-v` cuenta por la salida de error lo que hace el transporte con cada máquina: qué
orden se envía, si va por el túnel del daemon (`[daemon-rpc]`) o por SSH, y por qué
falla si falla. Las contraseñas y el material TLS salen tapados en ese registro.

La salida de lo que se pide va a la **salida estándar** y el registro a la **de error**,
así que `zfsmgr-cli ... > datos.tsv` no mezcla las dos.
