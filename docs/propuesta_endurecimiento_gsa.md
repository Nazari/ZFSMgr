# Propuesta de endurecimiento del GSA

## Objetivo

Reducir el riesgo operativo y de seguridad del `Gestor de snapshots` (GSA) sin perder la funcionalidad actual de snapshots automáticos y nivelación entre conexiones.

## Situación actual

El GSA se instala por conexión. Durante esa instalación, ZFSMgr genera un script para el sistema operativo remoto e incrusta dentro de él:

- la versión del script,
- la conexión `self`,
- la ruta de configuración,
- y, en Unix/macOS, un mapa estático de conexiones destino.

Ese mapa estático incluye actualmente, cuando aplica:

- host,
- puerto,
- usuario,
- password,
- ruta de clave SSH,
- indicador de `sudo`.

Consecuencia:

- el GSA no consulta dinámicamente las conexiones configuradas en el equipo donde corre ZFSMgr;
- usa la copia embebida en el payload instalado en la conexión remota;
- si cambian datos de una conexión, los GSA instalados pueden quedar desactualizados hasta reinstalarse o actualizarse.

## Riesgos principales del diseño actual

### 1. Secretos embebidos en el script

El script desplegado puede contener credenciales reutilizables:

- password SSH,
- password para `sudo`,
- ruta de clave privada.

Si el archivo remoto es legible por usuarios no autorizados, esas credenciales quedan expuestas.

### 2. Permisos de fichero demasiado amplios

En Unix/macOS el script se despliega actualmente como ejecutable global (`755`).

Si contiene datos de conexión sensibles, ese permiso es excesivo.

### 3. Verificación SSH debilitada

La ruta de nivelación remota usa opciones equivalentes a:

- `StrictHostKeyChecking=no`
- `UserKnownHostsFile=/dev/null`

Esto elimina protección efectiva frente a ataques MITM y permite aceptar cualquier huella remota.

### 4. Superficie de exposición mayor de la necesaria

Cada GSA instalado puede llevar embebidas más conexiones de las estrictamente necesarias para los datasets realmente programados en esa conexión.

### 5. Acoplamiento fuerte entre UI y payload desplegado

Cambios en:

- host,
- puerto,
- usuario,
- password,
- clave,
- `sudo`,

pueden invalidar payloads ya desplegados, aunque el usuario no toque la programación GSA de los datasets.

### 6. Paridad incompleta entre plataformas

La ruta Windows no tiene hoy el mismo nivel de resolución de destinos remotos que la ruta Unix/macOS. Eso debe considerarse una limitación funcional y también de diseño.

## Propuesta de arquitectura

### Fase 1. Separar script y configuración

Mantener un script GSA estable y desplegar aparte un fichero de configuración por conexión.

Ejemplo en Unix/macOS:

- script:
  - `/usr/local/libexec/zfsmgr-gsa.sh`
- configuración:
  - `/etc/zfsmgr/gsa-connections.conf`
  - `/etc/zfsmgr/gsa.conf`

Ejemplo en Windows:

- script:
  - `C:\\ProgramData\\ZFSMgr\\gsa.ps1`
- configuración:
  - `C:\\ProgramData\\ZFSMgr\\gsa-connections.json`

Ventajas:

- el script deja de contener secretos;
- actualizar conexiones no exige regenerar siempre el script;
- la rotación de credenciales se reduce a reescribir configuración.

### Fase 2. Reducir el mapa a mínimos

El fichero de conexiones desplegado en cada origen debe contener solo:

- la propia conexión local efectiva,
- y las conexiones que aparecen realmente como `Destino` en datasets GSA activos de ese origen.

No conviene desplegar el catálogo completo de conexiones.

### Fase 3. Endurecer permisos de ficheros

Unix/macOS:

- script: `700` o `750`
- configuración con secretos: `600`
- propietario: `root`

Windows:

- ACL restringida a `SYSTEM` y administradores
- sin herencia amplia a usuarios normales

### Fase 4. Sustituir SSH inseguro por huellas persistidas

Eliminar el uso de:

- `StrictHostKeyChecking=no`
- `UserKnownHostsFile=/dev/null`

Sustituirlo por:

- fichero `known_hosts` propio de ZFSMgr/GSA,
- pinning de huella por conexión,
- validación explícita al instalar o actualizar GSA.

### Fase 5. Reducir dependencia de passwords

Objetivo preferido:

- SSH por clave,
- `sudo -n` sin password interactiva,
- cuenta dedicada con privilegios mínimos necesarios.

Si no es posible:

- mantener los secretos solo en fichero de configuración protegido,
- nunca en el script,
- nunca en logs,
- nunca en argumentos visibles por procesos del sistema.

### Fase 6. Declarar compatibilidad por plataforma

Documentar explícitamente:

- qué soporta Linux,
- qué soporta macOS,
- qué limitaciones mantiene Windows.

## Propuesta funcional asociada

### Actualización automática controlada de GSA

Mientras siga existiendo mapa estático desplegado, ZFSMgr debe:

- detectar cambios de conexión relevantes,
- avisar claramente,
- actualizar automáticamente los GSA instalados afectados.

Esto reduce incoherencias operativas, pero no resuelve por sí mismo el problema de seguridad del payload.

### Diagnóstico visible en UI

Añadir un bloque de estado GSA por conexión con:

- versión de script desplegado,
- fecha de última actualización,
- si la configuración embebida o externa está al día,
- si faltan rutas SSH válidas,
- si la huella SSH del destino ha cambiado.

## Recomendación de implementación

Orden recomendado:

1. Separar script y configuración.
2. Endurecer permisos.
3. Sustituir SSH sin validación por `known_hosts` propio.
4. Reducir el mapa a conexiones realmente usadas.
5. Completar la paridad funcional de Windows o documentar su limitación definitiva.

## Resultado esperado

Con ese cambio, el GSA pasaría de ser un payload autosuficiente pero con secretos embebidos a un agente más mantenible y defendible:

- menos exposición de credenciales,
- menor necesidad de reinstalación completa,
- mejor trazabilidad,
- mejor resistencia frente a errores de configuración y ataques de red.
