# Logs de aplicación

Este panel usa un log combinado:

- Incluye eventos internos de la aplicación.
- Incluye salida de comandos SSH/PSRP de todas las conexiones.
- Las líneas de sesión remota aparecen con prefijo: `[SSH <conexion>]`.

## Carga inicial al arrancar

Al iniciar ZFSMgr:

- Se leen los logs persistidos (`application.log` y rotaciones `.1` ... `.5`).
- Se cargan en pantalla solo las últimas `N` líneas.
- `N` es el límite máximo de líneas configurado (menú `Menú > Logs`).
- Si no hay logs o están vacíos, no se muestra error.

## Presentación compacta en pantalla

Cada nueva línea se compara con la anterior visible.  
En pantalla se muestra:

- Solo los cambios de fecha.
- Solo los cambios de hora.
- Solo los cambios de conexión SSH.
- Solo los cambios de nivel de log.

Si no cambia ninguno de esos campos, se muestra `...` como cabecera compacta.

Formato visual:

- `<cambios> | <mensaje>`

## Persistencia

- El formato completo sigue guardándose en disco para trazabilidad.
- En pantalla se aplica la vista compacta para mejorar legibilidad.
