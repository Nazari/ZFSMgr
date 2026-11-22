# Protocolo de `zfsmgr-cli --serve`

`--serve` convierte el intérprete en un servicio para otro programa: la interfaz gráfica, o
cualquier guion que prefiera hablar JSON a leer tablas. Es el ÚNICO camino por el que la
interfaz gráfica actúa o lee: no lanza `ssh`, no lee `config.json`, no decide reglas de ZFS.
Lo que no se puede pedir por aquí, la interfaz no lo hace; se añade antes al CLI.

Este documento es el contrato. Cada punto está fijado por un caso de la batería
(`scripts/prueba_cli.py`, grupos `casos_servicio` y `casos_servicio_conexiones`). Cambiarlo
es cambiar las dos puntas a la vez: no hay compatibilidad con versiones anteriores.

## Arranque

```
zfsmgr-cli --serve [--lang es|en] [--config-dir <dir>] [--password-fd <n>]
```

- **Contraseña maestra.** Con `--password-fd`, se lee de ese descriptor. Sin él, es la
  PRIMERA línea de la entrada estándar (vacía si no hay nada cifrado). Nunca por argumento
  ni por entorno.
- Si no abre los secretos, sale con código 1 tras escribir
  `{"type":"error","text":"wrong master password"}`.
- Si abre, escribe una vez `{"type":"ready"}`. Puede traer `"warning"`: un aviso al cargar
  la configuración, para enseñarlo tal cual.
- `--lang` decide el idioma de TODOS los textos: errores, preguntas, avisos.

## Peticiones

Una por línea, en la entrada estándar:

```json
{"id": 7, "line": "ls zfsm://unib/ppal", "format": "json"}
```

| Campo | |
|---|---|
| `id` | Número que elige el cliente. Vuelve igual en todos los eventos de esa petición. |
| `line` | Una orden del intérprete, exactamente como se escribiría en el terminal. |
| `format` | Opcional: `json` (por omisión), `text` o `tsv`, solo para esta petición. |

- El servicio atiende **UNA orden cada vez**. El cliente no manda otra petición hasta recibir
  el `result` de la anterior. Lo único que se escribe mientras tanto son las respuestas a
  `confirm` y `secret`.
- **Para hacer cosas en paralelo, varios procesos.** Uno por conexión para las lecturas es lo
  razonable: una máquina lenta no detiene a las demás.
- Una línea que no es JSON, o sin `line`, devuelve un `result` con `rc` 2 y la causa en
  `stderr`. El servicio sigue.
- Con la entrada cerrada (EOF), o tras la orden `exit`, el servicio termina con código 0.

## Eventos

Uno por línea, en la salida estándar.

### `result`: la orden terminó

```json
{"type":"result","id":7,"rc":0,"stdout":"…","stderr":"…"}
```

- `rc` es el código de salida de la orden: 0 bien, distinto de 0 si falló. 2 es una petición
  o un uso incorrecto, y 127 una orden que no existe.
- `stdout` es la salida de la orden en el formato pedido. En `json` es UN documento. Su forma
  la documenta cada orden: ver «Salidas JSON» abajo.
- `stderr` son los avisos y errores, ya redactados para enseñarlos a una persona.
- La salida llega **entera al final**: no hay avance mientras la orden corre. Lo que tarda
  (envíos, breakdown, assemble…) se lanza como TRABAJO del daemon: la orden devuelve en el
  acto el id del trabajo, y el avance se lee con `jobs` o `job <id>`.

### `confirm`: la orden pregunta antes de seguir

```json
{"type":"confirm","id":7,
 "title":"Destruir ppal/user/srvs en unib", "irreversible":true,
 "lose":["200G de datos, con sus 2 datasets hijos (Correo, DocHistorica)",
         "sus 31 snapshots, uno hecho a mano: @antes-de-migrar",
         "está montado en /home/linarese/srvs: lo que use esos ficheros dejará de encontrarlos"],
 "keep":["la copia que trae fc16 (back/user/srvs)"],
 "notes":[],
 "type_to_confirm":"srvs",
 "text":"Destruir ppal/user/srvs en unib\nEsto no se puede deshacer.\n…"}
```

- Lo redacta el CLI, ya en el idioma de `--lang`, y calcula lo que puede preguntándolo a la
  máquina: tamaño, hijos, snapshots hechos a mano, dónde está montado, qué copias hay en otras
  máquinas. Lo que no puede preguntar no sale; nunca se supone.
- `text` es la pregunta entera en texto, la misma que enseña el terminal: para un cliente que
  no quiera pintar los campos.
- `irreversible` es lo que no se puede deshacer. `lose`, `keep` y `notes` pueden venir vacíos.
- Se contesta con `{"id":7,"answer":true}` o `false`.
- Con `type_to_confirm` no vacío, la respuesta tiene que traer la palabra escrita por la
  persona: `{"id":7,"answer":true,"typed":"srvs"}`. **La comprueba el CLI**: un `true` sin ella,
  o con otra, se toma por un no (`rc` 1, y en `stderr` el porqué).
- El cliente lo enseña tal cual: no lo reescribe ni decide por su cuenta si hace falta preguntar.

### `secret`: la orden necesita un secreto

```json
{"type":"secret","id":7,"text":"Contraseña de sudo para «unib»:"}
```

- Se contesta con `{"id":7,"secret":"…"}`, o `{"id":7,"secret":null}` para no darlo.
- Una misma orden puede pedir varios: la frase de cifrado se pide dos veces.

## Configuración compartida

El servicio lee `config.json` al arrancar, y **vuelve a leerlo** antes y después de cada
orden si su fecha o su tamaño han cambiado. Así ve las conexiones que otro proceso crea,
borra, conecta o desconecta: el terminal, u otro `--serve` de la misma interfaz. El cliente
no tiene que reiniciarlo ni avisarle.

## Salidas JSON

El cliente solo depende de las salidas listadas aquí; cada una tiene su caso en la batería.
Un campo nuevo se puede añadir sin avisar. Quitar o renombrar uno es un cambio de contrato.

| Orden | Documento |
|---|---|
| `ls zfsm://` | `{"connections":[{"id","name","connected","type","os","uid",…}]}`: las máquinas; `uid` es su identidad, la que lista `peers` |
| `ls zfsm://<conexión>` | `{"pools":[{"name","imported","size","free","health",…}]}` |
| `ls -r zfsm://<conexión>/<pool>` | `{"entries":[{"name","type","used","encryption","mounted",…}]}`, todo el árbol |
| `jobs [--history] zfsm://<conexión>` | `{"jobs":[{"id","state","type","description","bytes","started","finished","error",…}]}`; horas en ISO UTC |
| `peers zfsm://<conexión>` | `{"peers":[{"id","host","port","uid","role"}]}`; `role` es `pull`, `full` o `-` (daemon anterior a saberlo) |
| `ls <conexión/pool/dataset>` | `{"entries":[…]}`, una fila por hijo |
| `overview [-a]` | `{"machines":[…],"warnings":[…]}`; ver abajo |
| `probe <conexión>` | un objeto con lo que hay al otro lado; ver abajo |

*(Esta tabla crece con cada orden que use la interfaz nueva.)*

### `overview`

```json
{
  "machines": [
    {
      "connection": "local", "os": "Linux", "reachable": true,
      "daemon_version": "1.4.2.489679014", "daemon": "ok",
      "status": "warning",
      "pools": [{"name": "back", "health": "ONLINE", "size": "2.46T", "free": "749G",
                 "capacity": 70, "status": "ok"}],
      "schedules_checked": true,
      "schedules": [{"dataset": "back/user/srvs", "kind": "pull", "peer": "zfsm://unib/ppal/user/srvs",
                     "state": "ok", "last_snapshot": "GSA-daily-…", "last_snapshot_time": 1795000000,
                     "last_level_time": 1795000000, "job": "", "error": ""}],
      "jobs": [{"id": "…", "state": "running", "type": "gsa-pull", "snap": "…", "description": "…",
                "bytes": 2100000000, "rate": 94.1, "elapsed": 23}]
    }
  ],
  "warnings": [{"level": "warning", "connection": "local", "subject": "pool sback",
                "text": "el pool sback de local funciona con algún disco que falta o falla: …"}]
}
```

- Códigos fijos, iguales en cualquier idioma: `status` de máquina y de pool (`ok`, `warning`,
  `error`), `daemon` (`ok`, `outdated`, `ahead`, `unknown`), `kind` (`push`, `pull`, `local`) y
  `state` de programación (`ok`, `error`, `pending`, `paused`). Cada programación lleva `keep`,
  lo que guarda: `{"hourly","daily","weekly","monthly","yearly"}`. Las apagadas con `--off`
  salen como `paused`.
- Una máquina que no responde lleva `reachable: false` y `reason`, sin el resto de campos.
- `warnings` van ya redactados en el idioma de `--lang`, los errores primero. Qué es un aviso lo
  decide el CLI: el cliente solo los enseña.
- Para un panel, mejor `overview <conexión>` en el `--serve` de cada máquina, en paralelo, que
  un solo `overview -a`: así una máquina apagada no retrasa a las demás.

### `probe`

Lo que pregunta el asistente de conexión nueva: si la máquina responde y qué hay en ella. Va por
SSH y no al daemon, porque en una máquina nueva todavía no lo hay. Funciona también con una
conexión desconectada. `create` sin `--os` usa la misma detección del sistema.

```json
{
  "connection": "unib", "configured_os": "Linux", "reachable": true,
  "os": "Linux", "os_detail": "Arch Linux", "os_matches": true,
  "sudo": "ok", "sudo_detail": "",
  "openzfs": "ok", "openzfs_version": "2.4.4",
  "daemon": "ok", "daemon_version": "1.4.3.959941325",
  "status": "ok",
  "warnings": [{"level": "warning", "subject": "daemon", "text": "…"}]
}
```

- Códigos fijos: `os` (`Linux`, `macOS`, `FreeBSD`, `Windows`), `sudo` (`ok`,
  `wrong_password`, `needs_password`, `could_not_check`, `not_used`), `openzfs` (`ok`,
  `too_old`, `not_loaded`, `missing`, `unknown`), `daemon` (`ok`, `outdated`, `ahead`,
  `unknown`, `unreachable`, este con `daemon_detail`) y `status` (`ok`, `warning`, `error`).
- Sin respuesta: `reachable: false` y `reason`, sin el resto.
- `rc` es 1 cuando `status` es `error`: la conexión no sirve tal como está (el sistema no es el
  que dice, sudo no acepta la contraseña, falta OpenZFS…). Los `warnings` dicen qué hacer.
