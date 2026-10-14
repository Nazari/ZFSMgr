# Diseño y funcionamiento del GSA (snapshots programados)

## Qué es

El GSA hace snapshots automáticos por dataset, los poda según una retención por clase y,
si se pide, **nivela** cada dataset contra otro de otra máquina. Lo ejecuta el daemon de
la máquina que tiene el dataset (`zfsmgr-agent`), sin ningún cliente encendido. No hay
scripts, temporizadores de systemd ni ficheros de configuración aparte: todo vive en el
daemon y en las propiedades del dataset.

Se maneja con la orden `schedule` del CLI (`help schedule`) o con la pestaña
*Programaciones* de la ventana, que ejecuta las mismas órdenes.

## Configuración: propiedades del dataset

La programación son propiedades de usuario del propio dataset, así que sobrevive a
reinstalar el cliente y viaja con el pool si se exporta:

| Propiedad | Valor |
| --- | --- |
| `org.zfsmgr:schedule-enabled` | `on`/`off` |
| `org.zfsmgr:schedule-recursive` | `on`: un snapshot atómico del dataset y sus descendientes |
| `org.zfsmgr:schedule-hourly`, `-daily`, `-weekly`, `-monthly`, `-yearly` | cuántos se guardan de cada clase (0 = ninguno) |
| `org.zfsmgr:schedule-target` | destino de la nivelación: una URL `zfsm://conexión/pool/dataset` |
| `org.zfsmgr:schedule-level` | `on`: nivelar contra el destino tras cada snapshot |

```
schedule --hourly 24 --daily 7 --recursive --to zfsm://backup/tank/copia --level
schedule                 # enseña la de este dataset
schedule --off           # la apaga sin perder lo puesto
schedule --clear         # la borra
```

Fijar cualquier valor la activa. Las reglas se comprueban **antes** de escribir nada, con
funciones puras de `src/commands/gsa.cpp`:

- Una programación activada necesita alguna retención mayor que 0; si no, haría snapshots
  y los borraría.
- `--level` necesita `--to`, y `--to` solo admite una URL `zfsm://`.
- Dos programaciones activadas del mismo pool no pueden solaparse si una es recursiva
  (`isValidSet`).
- Un destino `zfsm://local/…` escrito desde otra máquina se reescribe con el nombre del
  cliente (`targetForScheduleMachine`): «local» para el daemon que lo ejecuta sería él
  mismo.

## Ejecución en el daemon

El daemon repasa las programaciones cada cinco minutos (`kGsaPasadaSegs`, en
`src/daemon/gsa.cpp`). En cada pasada y para cada dataset programado:

1. **Snapshot.** Si vence una clase, crea `GSA-<clase>-AAAAMMDD-HHMMSS`. Si el dataset no
   ha cambiado desde el último snapshot de **esa misma clase** (`written` = 0), no crea
   otro: renombra el anterior, para no gastar retención en algo que no aporta. Con
   `atime=on`, leer ficheros ya cuenta como cambio.
2. **Poda del origen** por clase. Nunca borra la base de la nivelación, aunque quede
   fuera de la retención: así un destino caído varios días se pone al día solo.
3. **Nivelación**, si `schedule-level=on`. Va aparte, como **trabajo** `gsa-level` del
   daemon (`decideLevel`), con un máximo de dos a la vez. Busca el último snapshot común
   **por guid** (no por nombre, porque el origen renombra), envía con `zfs send -I` al
   daemon del destino y poda allí los snapshots programados que el origen ya no tiene. Los
   snapshots manuales del destino no se tocan, y si el destino tiene snapshots propios
   posteriores al común no se envía nada, porque `zfs recv -F` los destruiría.

Para nivelar, el daemon del origen tiene que llegar por su cuenta al del destino:

```
peers --listen 0.0.0.0 --on zfsm://backup   # el destino atiende en la red (mTLS)
peers --push --on zfsm://servidor          # el origen recibe las credenciales de los demás
```

## Estado y observabilidad

El resultado de la última pasada de cada programación se guarda en
`/var/lib/zfsmgr/gsa-state.json` (`C:\ProgramData\ZFSMgr\agent\gsa-state.json` en
Windows) y se lee con el verbo `--dump-gsa-status`:

| Para | Orden |
| --- | --- |
| Lo programado en esta máquina, o en todas | `schedules`, `schedules -a` |
| Cómo fue la última pasada: snapshot, poda, nivelación y poda del destino, y si algo falló, cuándo y por qué | `schedule status`, `schedule status -a` |
| La pasada de un dataset, ahora | `schedule run`, y `--wait` para esperar a la nivelación |
| Las nivelaciones en curso | `jobs`, `job <id>`, `job cancel <id>` |
| El registro del daemon, con los eventos del GSA | `log --on zfsm://<conexión>` |

Al arrancar, el CLI comprueba el estado en segundo plano y lo dice en una línea; la zona
de estado enseña además el siguiente snapshot que toca (`soonestNext`). Una nivelación
cancelada no se reintenta sola: espera al siguiente snapshot o a otro `schedule run`.

`schedule run` es `--mutate-gsa-run-once <dataset>`: una pasada de un solo dataset. Es la
forma de probar el GSA sin lanzar pasadas globales.

## Referencias

- `help schedule` y `help schedules` en el CLI.
- [gramatica_cli.md](./gramatica_cli.md): las URL `zfsm://`.
- [daemon2daemon.md](./daemon2daemon.md): el canal entre daemons por el que va la nivelación.
- Históricos, para saber de dónde viene el diseño actual: [propuesta_gsa_cli.md](./propuesta_gsa_cli.md),
  [propuesta_endurecimiento_gsa.md](./propuesta_endurecimiento_gsa.md),
  [diseno_tecnico_endurecimiento_gsa.md](./diseno_tecnico_endurecimiento_gsa.md),
  [diseno_tecnico_gsa_freebsd.md](./diseno_tecnico_gsa_freebsd.md).
