# Propuesta: las instantáneas programadas (GSA) en el intérprete

Escrita el 2026-08-18. **Hechas las fases 1, 2 y 3**: las reglas en `src/base/gsa.{h,cpp}`,
las órdenes `schedule` y `schedules`, y `log`. Queda la 4, que ha cambiado de forma —ver
«GSA se fusiona con el daemon», abajo—.

Decidido: el verbo será **`schedule`** —también es como conviene llamarlo en la interfaz—,
y **`schedules` mira la máquina actual**, con `--all` para recorrerlas todas.

## Qué es GSA hoy

Comprobado leyendo el código, no de memoria:

- **La programación son PROPIEDADES del dataset**, de usuario, con prefijo
  `org.fc16.gsa:` — `activado`, `recursivo`, `horario`, `diario`, `semanal`, `mensual`,
  `anual`, `destino`, `nivelar`.
- **Quien las ejecuta es otro agente**, no el daemon: `/usr/local/libexec/zfsmgr-gsa.sh`,
  disparado por un temporizador de systemd en Linux (`zfsmgr-gsa.timer`) o un
  `LaunchDaemon` en macOS, con su configuración en `/etc/zfsmgr/gsa.conf`, sus destinos en
  `/etc/zfsmgr/gsa-connections.conf` y sus llaves en `/etc/zfsmgr/gsa_known_hosts`
  (`src/gsaversion.cpp`).
- **El daemon solo sirve de ventana**: `--dump-zfs-get-gsa-raw-all-pools`,
  `--dump-zfs-get-gsa-raw-recursive`, `--dump-gsa-connections-conf` y `--dump-gsa-log`.
  Ninguno de los cuatro lo usa el intérprete.
- **La interfaz valida antes de aplicar**, en `MainWindow::validatePendingGsaDrafts`
  (`src/mainwindow_dataset_props.cpp:471`).

## El punto de partida no es cero

Esto ya funciona hoy, sin tocar nada:

```
zfsm://local/pruebagsa/datos> set org.fc16.gsa:activado=on org.fc16.gsa:diario=7 org.fc16.gsa:horario=24
aplicadas 3 propiedades a pruebagsa/datos
```

Y las propiedades quedan puestas. O sea que **el intérprete ya puede programar
instantáneas**: lo que no puede es saber cómo se llaman las propiedades, qué valores
admiten, si lo que escribe es coherente, ni ver lo que hay programado en la máquina.

Eso fija el criterio de toda la propuesta: **el valor no está en escribir las propiedades,
está en las reglas, en el descubrimiento y en la visibilidad.** Una orden que se limitara a
envolver los `set` sería azúcar; lo que hace falta es lo que la interfaz tiene alrededor.

## Lo que se propone

### 1. Las reglas, a la capa base — y esto es lo primero

Las reglas viven hoy dentro de un método de `MainWindow`, en Qt, mezcladas con la cola de
cambios pendientes. Si el intérprete las vuelve a escribir, se separarán: es exactamente el
fallo que este repositorio ya ha pagado con la tabla de propiedades, con los motivos del
transporte y con las banderas de `zfs send`.

Se propone `src/base/gsa.{h,cpp}` —sin Qt, como `zfsprops`—, con:

```cpp
struct Programacion {
    bool activado{false};
    bool recursivo{false};
    bool nivelar{false};
    int horario{0}, diario{0}, semanal{0}, mensual{0}, anual{0};
    std::string destino;          // «Con::Pool/Dataset»
};

enum class FalloGsa { RetencionNoEntera, NivelarSinDestino, DestinoMalFormado,
                      DestinoSinConexion, ChocaConAscendienteRecursivo,
                      ChocaConDescendiente };

// Las propiedades tal cual las devuelve `zfs get` → estructura.
Programacion desdePropiedades(const std::map<std::string, std::string>&);
// Estructura → las propiedades que hay que escribir.
std::map<std::string, std::string> aPropiedades(const Programacion&);
// Y la comprobación, con motivo TIPADO, no con una frase.
bool valida(const Programacion&, const Contexto&, MotivoGsa& porQue);
```

Las reglas que hay que mover, tal y como están hoy en la interfaz:

1. las cinco retenciones son enteros `>= 0`;
2. `nivelar=on` exige `destino`;
3. `destino` tiene formato `Conexión::Pool/Dataset` **y la conexión tiene que existir**;
4. no se puede programar un dataset si un ascendiente ya tiene programación **recursiva**,
   ni marcar recursivo un ascendiente si un descendiente ya está programado.

La interfaz pasa a llamar a esto y borra su copia. Ese cambio, por sí solo, ya paga:
hoy la interfaz es el único sitio donde esas reglas existen.

### 2. `schedule` — ver y fijar la programación de un dataset

```
schedule                                  la programación de donde estás
schedule --daily 7 --hourly 24            la fija (y valida antes de escribir)
schedule --recursive --to oldlau::tank/copias --level
schedule --off                            activado=off, conservando las retenciones
schedule --clear                          borra las propiedades: como si nunca se programó
```

Con `--on <url>`, como todas. Dos decisiones que conviene tomar a propósito:

- **`--off` y `--clear` son cosas distintas** y las dos hacen falta: apagar sin perder la
  configuración es lo que uno quiere al depurar; borrarla del todo es lo que uno quiere al
  entregar la máquina. Una sola orden para ambas obligaría a recordar cuál de los dos
  comportamientos tiene.
- **Las opciones en inglés, las propiedades en español.** El catálogo del intérprete está
  en inglés (`--daily`), las propiedades son `org.fc16.gsa:diario` y no se pueden renombrar
  sin romper las instalaciones existentes. La traducción vive en `aPropiedades()`, en un
  solo sitio.

### 3. `schedules` — qué hay programado en esta máquina

Lo que hoy no se puede ver de ninguna manera desde el intérprete. Sale de
`--dump-zfs-get-gsa-raw-all-pools`, que ya existe, y se presenta como tabla —o sea que
`--format tsv|json` funciona gratis:

```
DATASET              ACT  REC  HORA  DÍA  SEM  MES  AÑO  DESTINO
tank/datos           sí   sí     24    7    4   12    0  oldlau::tank/copias
tank/media           sí   no      0    7    0    0    0  -
```

### 4. `log` — el registro del daemon

Donde queda lo que la máquina hizo por su cuenta, y donde quedarán las instantáneas
programadas cuando las haga el daemon. La interfaz lo enseñaba en su pestaña; desde el
intérprete no había forma de mirarlo.

La otra mitad que esta fase tenía —«una línea en `info` con la versión del agente GSA»—
**se cae**: `info` ya vuelca el `--health` entero, que trae la versión del agente, y con la
fusión no habrá un segundo agente cuya versión enseñar.

## GSA se fusiona con el daemon (decidido el 2026-08-18)

Antes de seguir se fue a mirar qué hay desplegado, y el terreno no era el que parecía:

- **`src/gsaversion.{h,cpp}` —la plantilla del guion y su versionado, 454 líneas— nunca ha
  estado en el build.** Ningún commit lo añadió a `CMakeLists.txt`, nadie en `src/` lo
  referencia y no se toca desde el 2026-04-01.
- **GSA no está instalado en ninguna máquina**: ni aquí —no existe
  `/usr/local/libexec/zfsmgr-gsa.sh` y `systemctl list-timers` da cero— ni en unibody, con
  el temporizador `inactive`. Las propiedades que se ven en `fc16/user` son restos.

O sea que la aplicación sabe LEER el estado de GSA y no puede instalarlo ni actualizarlo.
Así que no se trataba de fusionar dos cosas que funcionan, sino de decidir dónde vive el
planificador cuando se construya. **Va en el daemon**, por este orden de peso:

1. **La pata de «nivelar» es el grueso de GSA, y el guion la hace con `ssh` + `sshpass` +
   su propio `known_hosts` + `sudo -S` con la contraseña por tubería** — exactamente el
   patrón que el endurecimiento está borrando. El daemon ya hace esa pata, y mejor: mTLS,
   `--zfs-send-to-peer` con reanudación y seguimiento como trabajo.
2. **Un almacén de credenciales en vez de dos.** `gsa-connections.conf` guarda contraseñas
   en un fichero y duplica el trust-store.
3. **Un instalador en vez de dos.** `install-daemon` funciona en cinco plataformas; la
   prueba de que dos era demasiado es que el segundo murió como código muerto.
4. **Windows.** El daemon es nativo allí; un guion de shell no lo será nunca.

**Lo único que hay que conservar** es que la máquina sepa llegar al destino sin el cliente
delante —para eso existía `gsa-connections.conf`—. Fusionado, eso es «el daemon guarda las
credenciales de sus pares», y el mecanismo ya existe: «Exportar trust-store a esta
conexión».

Cómo hacerlo, con los dos riesgos que trae:

- **Las ejecuciones perdidas.** Un temporizador de systemd con `Persistent=true` recupera
  lo que no corrió con la máquina apagada; dentro del daemon hay que implementarlo. Se
  guarda la marca de última ejecución POR CUBO en el fichero de estado donde ya persiste
  los trabajos, y al arrancar se decide qué está vencido. Son pocas líneas y es donde
  viven los fallos: merece prueba propia.
- **El radio de daño.** Cada programación corre como TRABAJO del registro que ya existe
  —con su hilo y su cancelación—, nunca en el hilo del RPC. Y en código nativo con
  `execvp`, no lanzando shell, que es la regla que el daemon ya se aplica.

Lo que NO se hace: un intérprete de cron. Los cubos son fijos —horario, diario, semanal,
mensual, anual—, así que «qué toca» es aritmética sobre las marcas de última ejecución.

`src/gsaversion.cpp` se conserva de momento, fuera del build, como REFERENCIA de la lógica
de retención y poda que hay que portar. No se distribuye ni se compila.

## Orden de trabajo

| Fase | Qué | Por qué antes |
|---|---|---|
| 1 | ~~`src/base/gsa.*` + la interfaz pasa a usarlo~~ **HECHO** | sin esto, dos copias de las reglas |
| 2 | ~~`schedule` y `schedules`~~ **HECHO** | es el 90 % del uso |
| 3 | ~~`log`, el registro del daemon~~ **HECHO** | es lo que se mira cuando algo no salió |
| 4 | El daemon se queda con las programaciones | ver la sección de la fusión |

La fase 1 no cambia nada visible y es la que evita el problema de fondo; si solo se hiciera
una, sería esa.

## Lo decidido

1. **El verbo es `schedule`**, en inglés como el resto del catálogo, y ese mismo nombre es
   el que conviene usar en la interfaz.
2. **`schedules` mira la máquina actual**, y `--all` recorre todas las conexiones. Empezar
   por la actual evita que la orden más rápida se cobre el plazo de espera de una máquina
   apagada, que es justo lo que uno no quiere cuando está mirando por qué algo no salió.
3. **La fase 1 entró sola.** No cambia nada visible: la interfaz redacta los mismos
   mensajes, con las mismas claves, sobre las reglas ya compartidas.

## Qué quedó de la fase 1

`src/base/gsa.{h,cpp}`, sin Qt: `Programacion`, `desdePropiedades`/`aPropiedades`,
`valida` y `validaConjunto`, con motivos TIPADOS (`enum class Fallo`) en vez de frases.
`MainWindow::validatePendingGsaDrafts` se queda con lo que sí es suyo —reunir las
propiedades de la caché, del borrador y de lo que hay en vivo— y con la redacción en tres
idiomas, que ahora traduce el motivo tipado.

Al traerlas se ganó algo que no estaba en el plan: **ahora se pueden probar**. Antes había
que arrancar Qt y una ventana para llegar a ellas, así que nunca se habían probado. Son 26
comprobaciones en `tests/base_test.cpp`, cada regla con su control negativo — incluida la
que evita que `tank/datosviejos` cuente como hijo de `tank/datos` por empezar igual.
