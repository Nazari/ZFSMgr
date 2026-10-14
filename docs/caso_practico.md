# Mi información con ZFSMgr: un caso práctico

*English version: [case_study.md](case_study.md).*

Un servidor con Docker, una máquina de backup y un portátil cliente: cómo se gestiona y se protege todo con ZFSMgr, paso a paso y con tres máquinas: linux1, linux2 y macos1.

## El escenario

```
            macos1 (cliente, sin OpenZFS)
            zfsmgr-cli / ventana de ZFSMgr
               │                     │
          SSH + TLS              SSH + TLS
               ▼                     ▼
   ┌───────────────────────┐   ┌───────────────────────┐
   │ linux1 (servidor)     │   │ linux2 (backup)       │
   │ pool tank · 1,8 TB    │   │ pool backup · 2,5 TB  │
   │ Docker y servicios    │   │ réplica: backup/data  │
   │ zfsmgr-agent          │──▶│ zfsmgr-agent          │
   └───────────────────────┘   └───────────────────────┘
        nivelación nocturna daemon → daemon (mTLS, :47653)
```

linux1 guarda los datos y corre los servicios; linux2 tiene la réplica, que el daemon de linux1 mantiene al día sin nadie delante; macos1, sin OpenZFS, solo gobierna. Las dos máquinas con datos corren `zfsmgr-agent`, y el cliente les habla siempre a través de él: nunca ejecuta shell en ellas.

## 1. Dar de alta cada máquina

Todo se hace desde macos1: es donde está el cliente (`zfsmgr-cli` o la ventana de ZFSMgr), y desde ahí se gobiernan linux1 y linux2 por SSH. La primera vez que se arranca pide una **contraseña maestra**: con ella se cifran en disco las contraseñas y los certificados de cada máquina.

**Paso 1 — las conexiones.** En la raíz (`zfsm://`), `create` crea una conexión. La contraseña de sudo nunca va en la línea de órdenes: se teclea o entra por un descriptor.

```
create linux1 --type SSH --host linux1 --user admin --sudo --os linux
create linux2 --type SSH --host linux2 --user admin --sudo --os linux
ls
```

`ls` en la raíz enseña las conexiones; `cd linux1` entra en una y `ls` enseña sus pools. En la ventana es lo mismo: **Nueva conexión** en el menú de conexiones.

**Paso 2 — el daemon en cada máquina.** El trabajo lo hace `zfsmgr-agent`, un servicio que corre como root en cada máquina con OpenZFS y solo habla por TLS con certificados fijados. Una sola orden lo instala donde falta y lo pone al día donde se ha quedado atrás:

```
install-daemon -a --check     # qué haría, sin tocar nada
install-daemon -a
```

A partir de aquí ninguna orden ejecuta shell en las máquinas: todo es un verbo tipado del daemon.

**Paso 3 — que los daemons se hablen entre sí.** Los snapshots programados se nivelan de madrugada, sin el portátil encendido: el daemon de linux1 tiene que poder llegar por su cuenta al de linux2. Para eso, dos cosas:

```
peers --listen 0.0.0.0 --on zfsm://linux2   # linux2 RECIBE: su daemon atiende en la red
peers --push --on zfsm://linux1             # linux1 recibe las credenciales de las demás
peers --on zfsm://linux1                    # comprobar: debe listar linux2
```

`--listen` reinicia el daemon de linux2 para que atienda en la red, protegido por mTLS. Si linux2 solo debe ser alcanzable por una red concreta (una VPN, la LAN), el límite se pone en su cortafuegos. `--push` pregunta antes de mandar nada: entrega claves privadas, y quedan en un fichero de root con permisos 600.

macos1, sin OpenZFS, no necesita daemon ni `--listen`: solo es el cliente.

## 2. Organizar la información en datasets

La regla: **un dataset por cosa que se protege o se mueve distinto.** Un dataset es la unidad de snapshot, de copia, de cuota y de propiedades; un directorio no. Los contenedores de Docker son desechables y viven en `/` (ext4); lo que vale está en datasets ZFS que los servicios montan con `bind`.

| Dataset en linux1 (pool `tank`) | Tamaño | Qué guarda | Quién lo usa |
| --- | --- | --- | --- |
| `data/srvs/Correo` | 98 GB | Correo de los dominios propios, en Maildir | Dovecot y Roundcube |
| `data/srvs/DocHistorica` | 84 GB | Documentación histórica publicada | Nextcloud (su directorio de datos) |
| `data/srvs/compose` | 2 GB | Los `docker-compose.yml` y la configuración de cada servicio | Docker |
| `data/git` | 2,8 GB | Los repositorios | git |
| `data/Media/Musica` | 12 GB | Música | Navidrome (solo lectura) |
| `data/Media/Peliculas`, `Series` | 211 GB | Vídeo | Jellyfin (solo lectura) |
| `data/Media/Libros` | 7,6 GB | La biblioteca de Calibre | Calibre-web, Kavita |
| `data/Media/Juegos` | 745 GB | ROMs y emuladores | Samba |

Crear uno es `create` estando en su padre, con sus propiedades si hace falta:

```
cd linux1/tank/data/srvs
create Correo compression=zstd atime=off
ls #properties
```

`atime=off` en lo que se lee mucho (correo, multimedia) evita que leer cuente como escribir: afecta a los snapshots, porque un dataset que solo se ha leído no cambia. Mirar dentro sin salir del CLI: `ls #content` lista los ficheros del dataset, y `cd #content/2023` entra en un directorio.

## 3. ROMs: un dataset por consola (breakdown)

Hoy `Media/Juegos` es un único dataset de 745 GB, y dentro, `Roms/` tiene 69 directorios: uno por consola (`3ds`, `gba`, `dreamcast`, `cps2`…). Con todo en un dataset no se puede hacer snapshot solo de lo que cambia, ni poner una cuota o una compresión distinta a una consola. `breakdown` convierte un directorio en un dataset hijo **en el mismo sitio**: los emuladores y Samba siguen viendo las mismas rutas.

**Primero, mirar sin tocar nada.** `--list` enseña los directorios candidatos con lo que ocupan y cuántos ficheros tienen:

```
cd linux1/tank/data/Media/Juegos
breakdown --list --depth 2
```

**Después, en dos pasos.** Primero `Roms` pasa a ser un dataset; luego, dentro de él, cada consola. Los argumentos van en pares: directorio y nombre del dataset nuevo.

```
breakdown Roms Roms
cd Roms
breakdown gba gba snes snes psx psx dreamcast dreamcast
jobs
```

Cada `breakdown` es un **trabajo** del daemon de linux1: el intérprete queda libre y `jobs` enseña el avance. Cada byte se mueve una sola vez, y los originales se borran al final, cuando todas las copias están verificadas: si algo falla a mitad, lo de antes sigue intacto. Con 745 GB conviene ir por tandas de consolas, no las 69 de golpe.

**En la ventana:** clic derecho sobre el dataset → *Desglosar*. El diálogo enseña el árbol de directorios con sus tamaños, se marcan los que se quieren convertir y ejecuta la misma orden.

Una vez separadas, cada consola puede tener lo suyo: por ejemplo `set compression=zstd-19` en las que ya no se tocan, o snapshots solo de las que cambian, sin arrastrar los 745 GB enteros.

## 4. Libros: de un dataset por autor a uno solo (assemble)

El caso contrario. `Media/Libros/CalibreLibrary` tiene 2.791 autores; si cada uno fuera un dataset, serían 2.791 puntos de montaje, 2.791 series de snapshots y un `zfs list` interminable, para 7,6 GB que se gestionan juntos. Calibre los trata como carpetas de una biblioteca, y eso deben ser. `assemble` devuelve cada dataset hijo a ser un directorio corriente **en el mismo sitio**: Calibre-web y Kavita no notan nada.

```
cd linux1/tank/data/Media/Libros/CalibreLibrary
ls
assemble "A. A. Milne" "A. S. Byatt" "Agatha Christie"
```

Los nombres se dan relativos al dataset actual, y van entre comillas cuando llevan espacios. `assemble` no admite comodines, así que con miles de autores hay dos caminos:

- **La ventana:** clic derecho sobre `CalibreLibrary` → *Ensamblar*, **Seleccionar todo** y aceptar. Es la vía cómoda para este caso.
- **El CLI desde un guion:** listar los hijos con `--format tsv` y pasárselos a `assemble` por tandas.

Igual que `breakdown`, es un trabajo del daemon: se sigue con `jobs`, y si algo se corta a mitad, lo original sigue intacto. Antes de empezar, un snapshot del padre es un seguro barato: `create @antes-de-ensamblar -r`.

## 5. Snapshots programados y copia automática a linux2

Una programación dice cuántos snapshots de cada clase se guardan y, opcionalmente, a qué dataset de otra máquina se **nivelan** después de cada uno. La ejecuta el daemon de linux1 cada cinco minutos, sin el portátil encendido; la configuración son propiedades del dataset (`org.zfsmgr:schedule-*`), así que viaja con el pool.

Las dos primeras, para lo que más importa:

```
cd linux1/tank/data/srvs
schedule --daily 7 --recursive --to zfsm://linux2/backup/data/srvs --level
cd ../git
schedule --daily 7 --to zfsm://linux2/backup/data/git --level
```

`--recursive` en `srvs` cubre a la vez `Correo`, `DocHistorica` y `compose`, con un snapshot atómico de los tres. Nivelar deja en linux2 los mismos snapshots que en linux1: manda solo lo nuevo desde el último común (buscado por guid) y poda en linux2 lo que linux1 ya no guarda.

Una propuesta para el resto, según cuánto cambia cada cosa:

| Dataset | Retención | Nivela a linux2 | Por qué |
| --- | --- | --- | --- |
| `data/srvs` (recursivo) | 24 horarias, 7 diarias, 4 semanales, 12 mensuales | sí | Correo y documentación: lo que no se puede volver a conseguir |
| `data/git` | 24 horarias, 7 diarias | sí | Cambia a menudo y pesa poco |
| `Media/Musica`, `Libros`, `Peliculas`, `Series` (cada uno) | 4 semanales, 6 mensuales | sí | Cambian poco; el riesgo es borrar por error |
| `Media/Juegos` (recursivo) | 2 mensuales | no | 745 GB que se pueden volver a conseguir |

Dos programaciones del mismo pool no pueden solaparse si una es recursiva: por eso `Media` no lleva una propia y cada hijo tiene la suya. `schedule` lo comprueba antes de escribir nada.

Si el dataset no ha cambiado desde el último snapshot de una clase, no se hace otro: se renombra el anterior. Por eso `atime=off` importa en lo multimedia.

**Ojo con las bases de datos.** Nextcloud guarda sus ficheros en `srvs/DocHistorica`, pero su PostgreSQL vive en un volumen de Docker en `/`, que es ext4 y queda fuera de los snapshots. Para que una restauración sea coherente, conviene un volcado periódico de la base dentro de `srvs`, o llevar ese volumen a un dataset.

**En la ventana:** pestaña *Programaciones* del dataset, que ejecuta las mismas líneas `schedule`.

## 6. El día a día: saber que todo va bien

Al abrir `zfsmgr-cli` en macos1, la línea de abajo ya contesta las tres preguntas de cada día, sin teclear nada:

```
zfsm://linux1/tank/data>
siguiente: 00:00 linux1/tank/data/git · sin trabajos en curso
```

- **Cuándo toca el siguiente snapshot** y de qué dataset. Si algo falló, delante sale «programaciones con errores: N» y el detalle se escribe una vez al arrancar.
- **Qué trabajos corren** (una nivelación, un `breakdown`), con su avance.
- **Si algún daemon está anticuado** respecto al cliente: «daemon anticuado: linux2».

Para ver el detalle:

| Pregunta | Orden |
| --- | --- |
| ¿Cómo fue la última pasada de cada programación? | `schedule status -a` |
| ¿Qué se está haciendo ahora en todas las máquinas? | `jobs -a --detail` |
| ¿Qué falló esta semana? | `jobs -a --failed` |
| ¿Cómo va este trabajo? ¿Lo paro? | `job <id>`, `job cancel <id>` |
| ¿Qué hizo el daemon de linux1 esta noche? | `log --on zfsm://linux1 --lines 500` |
| ¿Están sanos los discos? | `status` en el pool |

Así se ve hoy `schedule status -a`:

```
CONEXIÓN  DATASET         ÚLTIMO SNAPSHOT            NIVELACIÓN           DESTINO                           ESTADO
linux1    tank/data/git   GSA-daily-20261014-000418  ok 2026-10-14 11:30  zfsm://linux2/backup/data/git     verde
linux1    tank/data/srvs  GSA-daily-20261014-000418  ok 2026-10-14 00:04  zfsm://linux2/backup/data/srvs    verde
```

Una programación en rojo dice qué paso falló (snapshot, poda o nivelación), cuándo y por qué. Por ejemplo, «cannot reach the daemon of linux2» si linux2 estaba apagada: no hay que hacer nada, porque la poda nunca borra la base común y la siguiente pasada se pone al día sola. Para forzarlo sin esperar: `schedule run --wait` en el dataset.

**En la ventana:** la pestaña *Programaciones* del dataset (con *Ejecutar ahora*), la caja *Jobs* y *Conexiones → Log del daemon* muestran lo mismo.

## 7. Recuperar: de un fichero a una máquina entera

Tres escenas, de la más frecuente a la peor.

**Un correo borrado por error.** El snapshot de anoche lo tiene. Se mira dentro sin montar nada y se copia de vuelta solo lo que hace falta:

```
cd linux1/tank/data/srvs/Correo@GSA-daily-20261014-000418
ls #content
cd #content/<carpeta-del-buzón>
cp zfsm://linux1/tank/data/srvs/Correo@GSA-daily-20261014-000418#content/<carpeta-del-buzón> zfsm://linux1/tank/data/srvs/Correo#content/recuperado
```

`cp` es un trabajo del daemon y conserva dueños, permisos, fechas y atributos extendidos. Con `--check` enseña qué haría sin tocar nada.

**Una actualización de Nextcloud que lo estropea todo.** Dos opciones. `clone` saca una copia del snapshot de ayer al lado, para mirarla sin tocar lo de ahora. `rollback` devuelve el dataset entero a ese snapshot, descartando lo posterior (hay que parar el contenedor antes):

```
cd linux1/tank/data/srvs/DocHistorica@GSA-daily-20261013-093056
clone tank/data/srvs/DocHistorica-ayer    # copia para mirar, al lado
rollback                                  # o volver del todo a ese estado
```

El `clone` va con nombre completo a propósito: con un nombre suelto colgaría de `DocHistorica` y aparecería dentro de los datos de Nextcloud.

**El disco de linux1 muere.** Todo lo programado está en linux2, en `backup/data`, con la misma historia de snapshots. Con el disco nuevo y el pool `tank` creado de nuevo, se manda de vuelta desde linux2:

```
cd linux2/backup/data/srvs
send zfsm://linux1/tank/data/srvs --from @GSA-daily-20261014-000418 -R
jobs -a
```

`-R` lleva el dataset con sus hijos y todos sus snapshots. Si la transferencia se corta, `send … --resume` la continúa donde se quedó, sin empezar de cero. Después se vuelve a poner la programación con `schedule`, y la primera nivelación encuentra la base común por guid: no vuelve a mandarlo todo.

**En la ventana:** el contenido de un snapshot se navega en su nodo del árbol, y *Clonar*, *Rollback* y *Enviar aquí desde* están en el menú contextual.

## 8. Mantenimiento

| Cuándo | Qué | Orden |
| --- | --- | --- |
| Al instalar una versión nueva del cliente en macos1 | Poner al día los daemons, y el `zfsmgr-cli` de las máquinas que lo tengan | `install-daemon -a --check`, luego `install-daemon -a` |
| Una vez al mes | Leer todo el pool y comprobar sumas: detecta un disco que empieza a fallar antes de perder nada | `scrub` en `linux1/tank` y en `linux2/backup` |
| Si `status` de un pool no dice ONLINE | Ver qué dispositivo falla y sus errores | `status`, y `clear` cuando esté resuelto |
| Si una nivelación se cortó | Continuarla, o descartar lo recibido a medias | `send … --resume` o `send … --discard-partial` |
| Si un `breakdown` o `assemble` se cortó | Devolver el dataset a su punto de montaje | `repair-mounts`, y con `--apply` lo arregla |

`install-daemon -a` no reinstala un daemon con trabajos en curso: espera a que no haya una nivelación en marcha, o lo dice. La zona de estado avisa de los daemons anticuados, así que no hace falta acordarse.

Antes de cualquier operación grande (un `breakdown` de cientos de GB, una actualización de Nextcloud), un snapshot manual es gratis y se guarda aparte de los programados: `create @antes-de-<lo-que-sea> -r`. La poda automática no toca los snapshots manuales.

## 9. Chuleta

La ventana ejecuta por debajo las mismas órdenes del CLI, así que lo que funciona en uno funciona en la otra. `help <orden>` da el detalle y los ejemplos de cada una.

| Para | Orden | En la ventana |
| --- | --- | --- |
| Dar de alta una máquina | `create linux1 --type SSH --host linux1 --user admin --sudo` | Conexiones → Nueva conexión |
| Instalar o actualizar los daemons | `install-daemon -a` | Conexiones → Reinstalar/Actualizar daemon |
| Que una máquina pueda recibir nivelaciones | `peers --listen 0.0.0.0 --on zfsm://linux2` | — |
| Darle a linux1 las credenciales de las demás | `peers --push --on zfsm://linux1` | Conexiones → Entregar credenciales de las demás máquinas… |
| Crear un dataset | `create Correo compression=zstd atime=off` | Crear dataset |
| Ver ficheros de un dataset o un snapshot | `ls #content` | Nodo de contenido del árbol |
| Directorio → dataset | `breakdown gba gba snes snes` | Desglosar |
| Datasets hijos → directorios | `assemble "A. A. Milne"` | Ensamblar |
| Programar snapshots y copia a linux2 | `schedule --daily 7 --to zfsm://linux2/backup/data/git --level` | Pestaña Programaciones |
| Estado de las programaciones | `schedule status -a` | Pestaña Programaciones |
| Hacer ahora la pasada de un dataset | `schedule run --wait` | Programaciones → Ejecutar ahora |
| Ver y parar trabajos | `jobs -a`, `job cancel <id>` | Caja Jobs |
| Registro del daemon | `log --on zfsm://linux1` | Conexiones → Log del daemon |
| Recuperar ficheros de un snapshot | `cp …@snap#content/x …#content/recuperado` | — |
| Copia de un snapshot para mirar | `clone tank/data/srvs/DocHistorica-ayer` | Clonar |
| Volver a un snapshot | `rollback` | Rollback |
| Mandar un dataset a otra máquina | `send zfsm://linux1/tank/data/srvs -R` | Enviar aquí desde…, con descendientes (-R) |
| Comprobar los discos | `scrub`, `status` | Menú del pool |

Las casillas con «—» son las que hoy solo tiene el CLI.
