# Diseño técnico de soporte GSA en FreeBSD

## Objetivo

Añadir soporte explícito para instalar y ejecutar el `Gestor de snapshots` (GSA) en conexiones FreeBSD.

El objetivo no es rediseñar el GSA, sino reutilizar el payload Unix actual y añadir la capa de despliegue, scheduler y validaciones propias de FreeBSD.

## Situación actual

Hoy el proyecto soporta explícitamente:

- Linux: `systemd`
- macOS: `launchd`
- Windows: `Task Scheduler`

El payload Unix del GSA ya es esencialmente portable a FreeBSD porque está implementado en `sh` y usa utilidades estándar:

- `sh`
- `zfs`
- `ssh`
- `sudo`
- `logger`
- opcionalmente `sshpass`

Eso hace que el soporte FreeBSD sea viable sin rehacer el núcleo del GSA.

## Alcance del soporte FreeBSD

El soporte debe cubrir:

1. detección de conexión FreeBSD en refresh/runtime
2. instalación del payload GSA Unix
3. activación/desactivación del scheduler nativo
4. comprobación de estado de instalación/activación/versionado
5. lectura del `GSA.log` desde la GUI
6. desinstalación limpia

No hace falta una variante nueva del payload del GSA salvo que aparezca una incompatibilidad real de shell o herramientas.

## Detección del sistema

Durante `refreshConnection(...)`, una conexión Unix debe clasificarse también como `FreeBSD` cuando `uname` o la cadena de sistema operativo lo indiquen explícitamente.

La clasificación final debería distinguir al menos:

- `Linux`
- `macOS`
- `FreeBSD`
- `Unix genérico`

La razón es que el scheduler y algunas rutas cambian.

## Scheduler propuesto

### Opción recomendada: `cron`

La opción más pragmática es usar `cron` con una entrada horaria.

Ventajas:

- disponible por defecto en FreeBSD
- fácil de instalar y borrar
- modelo similar al disparo actual por hora del GSA
- no exige integrarse con `periodic` ni crear infraestructura adicional

Formato propuesto:

- entrada en `/etc/crontab` o mejor en un fichero dedicado dentro de una ruta mantenible si se decide soportarlo
- ejecución a minuto `0` de cada hora

Ejemplo conceptual:

```cron
0 * * * * root /usr/local/libexec/zfsmgr-gsa.sh
```

### Opción alternativa: `periodic`

`periodic` es más nativo desde el punto de vista FreeBSD, pero menos adecuado para este caso porque:

- está más orientado a tareas diarias/semanales/mensuales
- para disparo horario complica innecesariamente el diseño

Conclusión:

- primera implementación: `cron`
- `periodic` solo si en el futuro hay una razón administrativa clara

## Rutas propuestas

### Script principal

Mantener la misma ruta Unix:

- `/usr/local/libexec/zfsmgr-gsa.sh`

### Configuración

Mantener la misma convención:

- `/etc/zfsmgr/gsa.conf`
- `/etc/zfsmgr/gsa-connections.conf`
- `/etc/zfsmgr/gsa_known_hosts`

### Runtime y logs

Hay dos opciones razonables:

1. mantener la ruta Linux actual:
   - `/var/lib/zfsmgr/GSA.log`
2. adoptar una convención más BSD:
   - `/var/db/zfsmgr/GSA.log`

Recomendación:

- usar `/var/db/zfsmgr`

Motivo:

- en FreeBSD, `var/db` encaja mejor como directorio de estado persistente de un servicio sencillo

Propuesta final:

- runtime dir: `/var/db/zfsmgr`
- log: `/var/db/zfsmgr/GSA.log`

## Permisos y ownership

Despliegue esperado:

- `/usr/local/libexec/zfsmgr-gsa.sh`: `root:wheel`, `700`
- `/etc/zfsmgr`: `root:wheel`, `700`
- `/etc/zfsmgr/gsa.conf`: `root:wheel`, `600`
- `/etc/zfsmgr/gsa-connections.conf`: `root:wheel`, `600`
- `/etc/zfsmgr/gsa_known_hosts`: `root:wheel`, `600`
- `/var/db/zfsmgr`: `root:wheel`, `700`
- `/var/db/zfsmgr/GSA.log`: creado por el propio script con permisos por defecto de root

## Dependencias mínimas

Obligatorias:

- `zfs`
- `sh`
- `ssh`
- `sudo`
- `logger`

Opcionales según configuración:

- `sshpass`

Validación previa de instalación o actualización:

- comprobar que `zfs` está disponible
- comprobar que `sudo` existe si la conexión lo requiere
- comprobar `sshpass` si alguna ruta de nivelación de ese GSA depende de password

## Estado GSA en FreeBSD

ZFSMgr debe poder informar de:

- instalado o no instalado
- activo o no activo
- versión desplegada
- conexiones dadas de alta en el GSA
- si necesita atención (`(*)`)

### Detección de instalación

Condiciones mínimas:

- existe `/usr/local/libexec/zfsmgr-gsa.sh`
- existe `/etc/zfsmgr/gsa.conf`

### Detección de versión

Igual que en Unix actual:

- extraer versión desde comentario del script
- compararla con la versión automática derivada por la app

### Detección de activo

Con `cron` no hay un estado "activo" tan rico como en `systemd` o `launchd`.

Propuesta pragmática:

- considerar `activo` si existe la entrada de `cron` esperada
- considerar `instalado pero no activo` si existe el payload pero no la entrada `cron`

## Instalación

Pasos de instalación/actualización GSA en FreeBSD:

1. crear `/etc/zfsmgr`
2. crear `/var/db/zfsmgr`
3. desplegar script principal
4. desplegar `gsa.conf`
5. desplegar `gsa-connections.conf`
6. desplegar `gsa_known_hosts`
7. fijar ownership y permisos
8. instalar o refrescar entrada de `cron`

La instalación de `cron` debe ser idempotente.

Recomendación:

- escribir un bloque identificado por comentarios ZFSMgr en el crontab de root
- reemplazar solo ese bloque

Ejemplo conceptual:

```text
# BEGIN ZFSMgr GSA
0 * * * * root /usr/local/libexec/zfsmgr-gsa.sh
# END ZFSMgr GSA
```

## Desinstalación

Debe:

1. borrar el bloque de `cron`
2. borrar script y configuración desplegada
3. opcionalmente conservar o borrar logs según decisión de producto

Recomendación inicial:

- borrar también el runtime dir del GSA en la desinstalación

## Integración con la GUI

### Menú de conexión

En conexiones FreeBSD, el menú `GSA` debe comportarse igual que en Linux/macOS:

- `Instalar gestor de snapshots`
- `Actualizar versión del Gestor de snapshots`
- `Activar GSA`
- `GSA actualizado y funcionando`

### Tab `GSA`

Debe leer:

- `/var/db/zfsmgr/GSA.log`

siempre con la misma ruta remota que corresponda a FreeBSD.

### Señalización de atención

Debe reutilizar la lógica ya existente:

- `(*)` si la versión es antigua
- `(*)` si el mapa de conexiones dadas de alta no coincide con el requerido

## Riesgos y matices

### `sshpass`

Puede no estar instalado por defecto en FreeBSD.

Consecuencia:

- ciertas rutas de nivelación o comprobaciones de conectividad seguirán fallando si dependen de password y el host no tiene `sshpass`

### `cron`

`cron` no ofrece introspección tan rica como `systemd` o `launchd`.

Consecuencia:

- la señalización de `activo` será más simple
- el diagnóstico de fallos deberá apoyarse más en el `GSA.log`

### `wheel`

A diferencia de Linux, el grupo esperado de root en FreeBSD es `wheel`, no `root`.

## Fases de implementación

### Fase 1

- detectar FreeBSD
- desplegar script/configs
- soportar runtime en `/var/db/zfsmgr`
- instalar entrada `cron`
- leer `GSA.log`

### Fase 2

- mejorar diagnóstico de `cron`
- endurecer validaciones previas de entorno
- revisar conectividad y documentación específica

## Criterio de aceptación

Se considerará soportado cuando, en una conexión FreeBSD:

1. ZFSMgr pueda instalar GSA
2. ZFSMgr pueda detectar su versión y si necesita actualización
3. ZFSMgr pueda activarlo mediante `cron`
4. el GSA cree snapshots horarios automáticos
5. la GUI pueda leer `GSA.log`
6. la desinstalación limpie el scheduler y los ficheros desplegados
