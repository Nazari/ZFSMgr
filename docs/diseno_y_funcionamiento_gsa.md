# Diseño y funcionamiento del GSA

## Objetivo

El `Gestor de snapshots` (GSA) es el componente que programa snapshots ZFS automáticos por dataset y, cuando procede, ejecuta nivelación (`zfs send | zfs recv`) hacia otro destino.

El GSA se instala por conexión y se ejecuta en la máquina de esa conexión, no en la máquina donde corre la interfaz gráfica de ZFSMgr.

## Modelo operativo

### Dónde se ejecuta

Si desde ZFSMgr se programa una política sobre un dataset de la conexión `B`, el trabajo periódico lo ejecuta el GSA desplegado en `B`.

Ejemplo:

- ZFSMgr corre en `A`
- Se programa `B::pool/ds`
- El GSA que crea snapshots y hace nivelación es el de `B`

### Qué datasets procesa

El GSA recorre datasets `filesystem` y lee propiedades de usuario `org.fc16.gsa:*`.

Propiedades relevantes:

- `org.fc16.gsa:activado`
- `org.fc16.gsa:recursivo`
- `org.fc16.gsa:horario`
- `org.fc16.gsa:diario`
- `org.fc16.gsa:semanal`
- `org.fc16.gsa:mensual`
- `org.fc16.gsa:anual`
- `org.fc16.gsa:nivelar`
- `org.fc16.gsa:destino`

### Qué hace en cada ejecución

Para cada dataset programado:

1. Evalúa si hay clases vencidas (`hourly`, `daily`, `weekly`, `monthly`, `yearly`).
2. Crea snapshots `GSA-<clase>-YYYYMMDD-HHMMSS`.
3. Aplica poda según la retención configurada.
4. Si `nivelar=on` y hay `destino`, ejecuta la ruta `send/recv` correspondiente.

## Programación recursiva

Si un dataset tiene `recursivo=on`:

- el GSA crea el snapshot con `zfs snapshot -r`
- los hijos quedan cubiertos por ese snapshot recursivo
- los hijos no deben tener una programación independiente equivalente

Defensas actuales en runtime:

- si un dataset está cubierto por un ancestro con GSA recursivo activo, el hijo se omite
- si en la ejecución actual ya se lanzó un snapshot recursivo de un ancestro, los descendientes se omiten explícitamente
- si un snapshot ya existe, se registra como `skip` en lugar de dejar fallar `zfs snapshot`

## Destino y nivelación

El formato funcional de `Destino` es:

- `Conexion::pool/dataset`

Ejemplos:

- `BackupNAS::tank/copias/proyecto`
- `Local::backup/apps/app1`

Interpretación:

- `Conexion` es el nombre de una conexión configurada en ZFSMgr
- `pool/dataset` es el dataset destino en esa conexión

## Cómo resuelve conexiones el GSA

El GSA no lee dinámicamente los `conn_*.ini` del equipo donde corre la GUI.

En su lugar, cuando ZFSMgr instala o actualiza el GSA en una conexión, despliega:

- un script estable del GSA
- una configuración principal
- un mapa mínimo de conexiones destino realmente usadas por esa conexión origen
- un fichero `known_hosts` propio del GSA

Consecuencia operativa:

- si cambian datos relevantes de una conexión (`host`, `port`, `usuario`, `password`, clave SSH, `sudo`), los GSA desplegados pueden quedar obsoletos
- ZFSMgr intenta mitigarlo actualizando automáticamente los GSA instalados afectados cuando se crea o edita una conexión
- además, ZFSMgr compara las conexiones realmente requeridas por las programaciones activas con las conexiones dadas de alta dentro del GSA desplegado; si no coinciden, la conexión queda marcada para actualización

## Versionado del GSA

La versión visible del GSA tiene formato:

- `ZFSMgrAppVersion.GsaPayloadFingerprint`

Ejemplo:

- `0.10.0rc1.854224243`

Reglas actuales:

- la parte base (`0.10.0rc1`) coincide con la versión de la aplicación
- el sufijo numérico se deriva automáticamente del hash del código fuente que contiene el payload y el despliegue del GSA
- no se incrementa manualmente

Consecuencia práctica:

- si cambia el payload o el esquema de despliegue del GSA, la versión local cambia automáticamente
- cualquier conexión con una versión anterior pasa a requerir actualización aunque la versión base de la app siga siendo la misma

## Señalización en la UI

En la tabla `Conexiones`, ZFSMgr añade `(*)` al nombre de una conexión si su GSA necesita atención.

Actualmente eso ocurre cuando:

- la versión instalada del GSA es anterior a la versión esperada por la aplicación
- o el GSA desplegado no contiene las conexiones destino realmente requeridas por las programaciones activas de esa conexión

Efectos visibles:

- la fila de la conexión muestra `(*)`
- el tooltip de la fila incluye:
  - conexiones dadas de alta en el GSA
  - conexiones requeridas por sus programaciones actuales
  - motivo de atención
- el menú contextual `GSA` pasa a ofrecer `Actualizar versión del Gestor de snapshots`

## Diseño desplegado actualmente

### Unix y macOS

Script principal:

- `/usr/local/libexec/zfsmgr-gsa.sh`

Configuración:

- `/etc/zfsmgr/gsa.conf`
- `/etc/zfsmgr/gsa-connections.conf`
- `/etc/zfsmgr/gsa_known_hosts`

### Linux

Scheduler:

- `systemd`
- unit files:
  - `/etc/systemd/system/zfsmgr-gsa.service`
  - `/etc/systemd/system/zfsmgr-gsa.timer`

Runtime y log:

- `/var/lib/zfsmgr`
- log:
  - `/var/lib/zfsmgr/GSA.log`

Permisos desplegados esperados:

- `/usr/local/libexec/zfsmgr-gsa.sh`: `root:root`, `700`
- `/etc/zfsmgr`: `root:root`, `700`
- `/var/lib/zfsmgr`: `root:root`, `700`

### macOS

Scheduler:

- `launchd`
- plist:
  - `/Library/LaunchDaemons/org.zfsmgr.gsa.plist`

Runtime y log actuales:

- configuración bajo `/etc/zfsmgr`
- log GSA en el directorio configurado por el payload desplegado

### Windows

Scheduler:

- `Task Scheduler`

Notas:

- La ruta Windows no tiene todavía la misma madurez funcional que Unix/macOS para todas las variantes de nivelación remota.
- Debe considerarse soporte parcial frente a Linux/macOS en esta parte concreta.

## Seguridad del diseño actual

El diseño actual está más endurecido que en versiones anteriores, pero no debe considerarse perfecto.

### Mejoras ya aplicadas

- separación entre script principal y configuración desplegada
- mapa de conexiones reducido a destinos realmente usados
- fichero `known_hosts` propio del GSA
- en Linux, runtime y logs en ruta de sistema
- permisos restrictivos y ownership explícito `root:root` en Linux
- logs GSA leídos desde la GUI mediante la ruta adecuada del sistema

### Riesgos que siguen existiendo

- el mapa de conexiones desplegado todavía puede contener material sensible dependiendo del tipo de autenticación configurado
- el modelo Windows no está endurecido al mismo nivel
- sigue existiendo dependencia entre cambios de conexiones y payloads ya desplegados

Documentos relacionados:

- [propuesta_endurecimiento_gsa.md](./propuesta_endurecimiento_gsa.md)
- [diseno_tecnico_endurecimiento_gsa.md](./diseno_tecnico_endurecimiento_gsa.md)

## Logs y observabilidad

El GSA registra eventos de alto nivel, por ejemplo:

- arranque del script y versión
- evaluación de cada dataset programado
- clases vencidas
- intento de snapshot
- snapshot creado
- `skip` por cobertura de ancestro recursivo
- `skip` por snapshot ya existente

Ejemplos reales de líneas esperadas:

- `GSA start version 0.10.0rc1.854224243`
- `GSA evaluate tank1/user: recursive=on hourly=3 ... due=hourly`
- `GSA snapshot attempt for tank1/user: class=hourly recursive=on snap=GSA-hourly-...`
- `GSA snapshot created for tank1/user: GSA-hourly-...`
- `GSA skip for tank1/user/bin: cubierto por snapshot recursivo ya realizado en esta ejecución`

## Qué implica para el usuario

### Cuando se cambia una conexión

Si se modifican datos de acceso o de red de una conexión, puede ser necesario redeplegar GSA en conexiones origen que dependan de ella como destino.

ZFSMgr intenta hacerlo automáticamente, pero es importante entender el motivo:

- el GSA usa configuración desplegada, no consulta dinámica de conexiones desde la GUI

### Cuando no aparece actividad GSA

La comprobación correcta no es solo la UI. Hay que verificar:

1. que el scheduler nativo está activo
2. que el GSA instalado tiene la versión esperada
3. que el log GSA refleja evaluaciones y snapshots
4. que las propiedades `org.fc16.gsa:*` están realmente en el dataset correcto

### Comandos útiles

Linux:

```bash
sudo systemctl start zfsmgr-gsa.service
sudo systemctl status zfsmgr-gsa.service
sudo systemctl status zfsmgr-gsa.timer
sudo tail -n 100 /var/lib/zfsmgr/GSA.log
```

macOS:

```bash
sudo launchctl kickstart -k system/org.zfsmgr.gsa
sudo launchctl print system/org.zfsmgr.gsa
```

## Limitaciones actuales

- Windows no tiene paridad completa con Unix/macOS en nivelación remota.
- El GSA sigue dependiendo de configuración desplegada y versionada por conexión.
- La seguridad del payload ha mejorado, pero aún existe margen de endurecimiento adicional.

## Estado actual recomendado

La referencia de diseño y funcionamiento vigente del GSA para el proyecto debe tomarse de este documento y de los dos documentos de endurecimiento relacionados.
