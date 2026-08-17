# `zfsm://` — nombrar cualquier elemento del árbol

## La forma

```
zfsm://<conexión>/<pool>/<dataset>[@snapshot][#<sección>[/<detalle>]]
```

**Una regla:** antes de `#` está el objeto ZFS; después de `#`, la ruta *dentro* de ese
objeto, con los mismos nombres que se ven en el árbol.

```
zfsm://unibody                                    la conexión
zfsm://unibody#daemon                             su pestaña Daemon
zfsm://unibody/sback                              el pool, que TAMBIÉN es un dataset
zfsm://unibody/sback@antes                        su snapshot
zfsm://unibody/sback/user                         un dataset
zfsm://unibody/sback/user@ayer                    un snapshot
zfsm://unibody/sback/user#properties/compression  una propiedad
zfsm://unibody/sback/user#permissions             sus permisos
zfsm://unibody/sback/user#content/docs/a.pdf      un fichero dentro
```

## Por qué así

Casi nada está inventado, y es deliberado: **un nombre ZFS ya es una ruta separada por
`/`** y **un snapshot ya usa `@`**. Lo único que se decide aquí es que la **conexión es
la autoridad** —el «dónde», que es para lo que la autoridad existe en una URL— y que el
primer tramo de la ruta es el pool.

El fragmento carga con todo lo que está «dentro» —propiedades, permisos, ficheros— porque
eso es lo que un fragmento significa en cualquier URL. Se descartó `$propiedad` como
marcador aparte: habría tres formas de decir «dentro de esto» en vez de una, y se recuerda
mejor una regla que tres.

Se descartó `<conexión>::<dataset>` por lo mismo: si la conexión es la autoridad, el
separador ya lo pone la URL, y `::` en una autoridad recuerda a IPv6, que es justo donde
confunde.

### Lo que sale gratis por seguir el estándar

- **Espacios y caracteres raros.** ZFS admite espacios en los nombres; `%20` ya es la
  respuesta.
- **Referencias relativas.** Estando «en» una conexión, `sback/user` se resuelve solo.
  Hará cómodo un CLI interactivo, cuando llegue.
- **Todo el mundo sabe leerlas.**

## Un pool es un dataset

No hay clase «pool» aparte, y no es un olvido. En ZFS el pool **es** un dataset:
`zfs list sback` lo devuelve y `zfs snapshot winpool@snap1` funciona —comprobado contra
una máquina real—. Tenerlo como clase distinta era una mentira del modelo, y además
impedía nombrar el snapshot de un pool, que es un caso normal.

Las clases son **conexión, dataset y snapshot**. Para saber si un dataset es la raíz de su
pool está `esRaizDePool()`, sin fingir que sea otra cosa.

## Todo lo público va en inglés

**Los literales** —`content`, `properties`, `permissions`, `info`, `daemon`—, **la API**
—`connection`, `section`, `detail`, `isPoolRoot()`, `zfsName()`— y **los nombres de campo
que saca `url parse`**, porque un guion que haga `grep dataset` depende de ellos.

Es la excepción del proyecto y está dicha en la cabecera: el resto de `base/` es interno y
sigue en español. `zfsm://` no lo es.

### Por qué los literales

Aunque el árbol se vea en español o en chino: una URL es un **identificador**, no texto para leer. Si el literal dependiera del idioma
de quien la escribió, la misma cosa tendría tres nombres y ninguno serviría para guardarla
ni compararla.

## Decisiones del analizador

**Estricto con lo que puede ocultar un fallo, tolerante con lo que no.**

Rechaza: otro esquema, conexión vacía, `@` sin nombre, dos `@`, un snapshot colgando de un
pool en vez de un dataset, `/` dentro del snapshot, tramo vacío (`a//b`), `#` sin sección,
y codificación por-ciento inválida o a medias.

Acepta: una **sección desconocida** —el árbol puede ganar pestañas y no se va a tocar este
fichero cada vez—, una **barra final** —quien copia del árbol la arrastra sin querer—, y
mayúsculas en el esquema y la sección.

Dos que merecen explicación:

- **Un `%` suelto es un error, no un `%` literal.** Si se dejara pasar, `100%` y `100%41`
  significarían cosas distintas según el humor del analizador.
- **`#` sin nada detrás se rechaza** en vez de ignorarse: es un error de quien escribe, y
  tragárselo daría por buena una URL que no nombra lo que su autor creía.

**Ida y vuelta garantizada:** `parse(format(x)) == x` para toda URL válida, con caso de
prueba. Es lo que impide que las dos mitades se separen con el tiempo. `formatZfsmUrl`
devuelve la forma **canónica**, que es la que hay que guardar o comparar — no la tecleada.

## Estado

**Solo nombra.** `src/base/zfsmurl.{h,cpp}`, sin Qt, con 45 casos de prueba. Y una orden
en el CLI para verlo:

```
zfsmgr-cli url parse "zfsm://unibody/sback/user@ayer#content/docs/a.pdf"
```

**Resolver** —ir a buscar lo que la URL nombra, abrirlo en el árbol, aceptarlo desde la
línea de órdenes del sistema, o que otra aplicación pueda lanzarla— viene después. Se
construye encima sin cambiar nada de esto: por eso el análisis devuelve una estructura y
no una cadena troceada a medias.
