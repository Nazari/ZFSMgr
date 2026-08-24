# Logs

**El log de cada conexión va bajo su panel**, no al pie de la ventana. Antes estaban todos
en pestañas abajo, junto a un `Log combinado`: mirar el de una máquina era buscar su
pestaña, y con dos paneles sobre dos máquinas, ir y volver entre dos pestañas.

Cada panel tiene dos:

- `Log · <máquina>`: salida técnica de los comandos de esa máquina.
- `Daemon · <máquina>`: log de su daemon (`/var/lib/zfsmgr/daemon.log`, o
  `C:\ProgramData\ZFSMgr\agent\daemon.log` en Windows) y botón `Heartbeat`.

Si la misma conexión está elegida en los dos paneles, los dos enseñan el mismo log: no se
duplica el texto ni se reparten las líneas.

El `Log combinado` se retiró. El log de la **aplicación** sigue escribiéndose en disco y se
copia desde `Ajustes ▸ Logs ▸ Copiar`.

Abajo del todo quedan dos cajas: `Cambios sin aplicar` y `Transferencias`.

## Pestaña Daemon

- Muestra el log del daemon remoto leído de forma incremental.
- El botón `Heartbeat` envía un ping al daemon para confirmar que responde.
- El log se actualiza al detectar un evento ZED o al pulsar `Heartbeat`.
- El log no se borra al refrescar la conexión; solo se resetea si el daemon ha sido reinstalado.
- Los fallos de daemon-rpc aparecen en los logs como `daemon-rpc:fallback` o
  `daemon-rpc:skip`, seguidos de una etiqueta estable que dice de qué tipo fue el fallo
  (`tls-handshake`, `conexion-rechazada`, `tunel-ocupado`…). Esa etiqueta no se traduce a
  propósito: es lo que se busca con `grep` en un registro que puede venir de una máquina
  configurada en otro idioma.
- **Ante un fallo de TLS, ZFSMgr NO reinstala el daemon ni rehace el material por su
  cuenta**: marca la conexión para atención y espera. Reaprovisionar regeneraría el
  material TLS y perpetuaría el bucle fallo → reinstalación → fallo. La reinstalación
  automática solo ocurre cuando el motivo es una desalineación de versión o de API.

## Caja Transferencias

- Muestra una fila por cada trabajo en marcha: `Enviar` y `Nivelar` entre daemons, y también
  `Desde Dir` cuando va por el árbol entre daemons.
- Cada fila incluye: estado, datasets origen/destino, bytes transferidos, velocidad y tiempo.
- Estados posibles: `running`, `done`, `failed`, `cancelled`.
- El botón `Refrescar` fuerza una consulta de estado a los daemons.
- El botón `Cancelar seleccionado` envía `SIGTERM` al proceso `zfs send` del job seleccionado.
- Los jobs en curso se recuperan automáticamente al reconectar.

## Caja Cambios sin aplicar

- Enumera, línea a línea, lo que van a hacer `Aplicar cambios` y `Descartar cambios`: cada
  propiedad editada y cada delegación de permisos tocada, con su conexión y su objeto.
- Sirve **solo** a los borradores de propiedades y de permisos: las acciones no se encolan,
  se ejecutan al pulsarlas.
- `Descartar cambios` avisa de qué se pierde antes de tirarlo.

## Carga inicial al arrancar

Al iniciar ZFSMgr:

- Se leen los logs persistidos (`application.log` y rotaciones `.1` ... `.5`).
- Se cargan en pantalla solo las últimas `N` líneas.
- `N` es el límite máximo de líneas configurado en el menú `Ajustes ▸ Logs`.
- Si no hay logs o están vacíos, no se muestra error.

## Presentación compacta en pantalla

Cada nueva línea se compara con la anterior visible.  
En pantalla se muestra:

- Solo los cambios de fecha.
- Solo los cambios de hora.
- Solo los cambios de conexión.
- Solo los cambios de nivel de log.

Si no cambia ninguno de esos campos, se muestra `...` como cabecera compacta.

Formato visual:

- `<cambios> | <mensaje>`

## Persistencia

- El formato completo sigue guardándose en disco para trazabilidad.
- En pantalla se aplica la vista compacta para mejorar legibilidad.
