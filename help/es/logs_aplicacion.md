# Logs de aplicación

La parte inferior de la ventana usa pestañas:

- `Transferencias`: los trabajos en marcha, con su progreso y un botón para cancelarlos.
  Antes esta pestaña era `Cambios pendientes` y guardaba órdenes a la espera de que
  usted las aplicara; ahora las órdenes se ejecutan al pulsarlas.
- `Ajustes`: opciones de log y confirmación de acciones.
- `Log combinado`: log principal de aplicación.

Dentro del `Log combinado`, **cada conexión** tiene además sus propias sub-pestañas:

- `Terminal`: salida técnica de los comandos de esa máquina.
- `Daemon`: log de su daemon (`/var/lib/zfsmgr/daemon.log`, o
  `C:\ProgramData\ZFSMgr\agent\daemon.log` en Windows) y botón `Heartbeat`.

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

## Pestaña Transferencias

- Muestra una fila por cada trabajo en marcha: `Copiar` y `Nivelar` entre daemons, y también
  `Desde Dir` cuando va por el árbol entre daemons.
- En esa misma zona están los botones `Aplicar cambios` y `Deshacer cambios`, que sirven
  **solo** a los borradores de propiedades y de permisos: las acciones ya no se encolan.
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
