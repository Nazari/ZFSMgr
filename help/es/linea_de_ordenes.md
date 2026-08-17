# Línea de órdenes (`zfsmgr_cli`)

ZFSMgr trae una herramienta de terminal que hace lo mismo que la ventana, contra las
**mismas conexiones**: lee `config.json` y `trust-store.json` del mismo sitio, habla por
el mismo túnel cifrado y ejecuta los mismos verbos del agente. No es un programa aparte
con su propia configuración.

En Windows el instalador la deja en el `PATH` —si deja marcada esa casilla—, así que se
llama por su nombre desde `cmd` o PowerShell. En Linux y macOS **todavía no se instala
junto a la aplicación**: se ejecuta desde donde se haya compilado.

## Dos formas de usarla

**Con una orden**, para guiones:

```sh
zfsmgr_cli connections list
zfsmgr_cli --format json connections list | jq '.connections[] | select(.tls == false)'
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
zfsmgr_cli --password-fd 3 connections list  3< <(pass show zfsmgr)
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
así que `zfsmgr_cli ... > datos.tsv` no mezcla las dos.
