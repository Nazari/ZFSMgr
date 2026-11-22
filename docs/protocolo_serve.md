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
{"type":"confirm","id":7,"text":"Se va a DESTRUIR …"}
```

- Se contesta con `{"id":7,"answer":true}` o `false`.
- El texto ya explica qué se pierde. El cliente lo enseña tal cual: no lo reescribe ni
  decide por su cuenta si hace falta preguntar.

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
| `ls <conexión/pool/dataset>` | `{"entries":[…]}`, una fila por hijo |

*(Esta tabla crece con cada orden que use la interfaz nueva.)*
