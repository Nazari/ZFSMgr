# Logs de aplicación

La parte inferior de la ventana usa pestañas:

- `Ajustes`: opciones de log y confirmación de acciones.
- `Log combinado`: log principal de aplicación.
- `Terminal`: salida técnica de comandos locales/remotos.
- `Daemon`: log del daemon remoto (`/var/lib/zfsmgr/daemon.log`) y botón `Heartbeat`.
- `Transferencias`: lista de jobs de transferencia en background (Copy/Level daemon-a-daemon).

## Pestaña Daemon

- Muestra el log del daemon remoto leído de forma incremental.
- El botón `Heartbeat` envía un ping al daemon para confirmar que responde.
- El log se actualiza al detectar un evento ZED o al pulsar `Heartbeat`.
- El log no se borra al refrescar la conexión; solo se resetea si el daemon ha sido reinstalado.

## Pestaña Transferencias

- Muestra una fila por cada job de transferencia (Copy/Level daemon-a-daemon).
- Cada fila incluye: estado, datasets origen/destino, bytes transferidos, velocidad y tiempo.
- Estados posibles: `running`, `done`, `failed`, `cancelled`.
- El botón `Refrescar` fuerza una consulta de estado a los daemons.
- El botón `Cancelar seleccionado` envía `SIGTERM` al proceso `zfs send` del job seleccionado.
- Los jobs en curso se recuperan automáticamente al reconectar.

`Log combinado`:

- Incluye eventos internos de la aplicación.
- Incluye salida de ejecución relevante con formato compacto.

## Carga inicial al arrancar

Al iniciar ZFSMgr:

- Se leen los logs persistidos (`application.log` y rotaciones `.1` ... `.5`).
- Se cargan en pantalla solo las últimas `N` líneas.
- `N` es el límite máximo de líneas configurado en la pestaña `Ajustes`.
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
