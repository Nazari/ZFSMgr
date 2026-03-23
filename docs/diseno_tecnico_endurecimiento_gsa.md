# Diseño técnico de endurecimiento del GSA

## Objetivo

Rediseñar el despliegue del `Gestor de snapshots` (GSA) para:

- eliminar secretos embebidos en el script principal,
- reducir el acoplamiento entre cambios de conexión y payload instalado,
- endurecer permisos y confianza SSH,
- mantener compatibilidad funcional con la programación automática actual.

Este documento concreta la propuesta de [propuesta_endurecimiento_gsa.md](./propuesta_endurecimiento_gsa.md).

## Alcance

Incluye:

- Linux
- macOS
- Windows

Prioridad real de implementación:

1. Linux y macOS
2. Windows

## Situación actual resumida

Hoy ZFSMgr despliega un script GSA por conexión.

Ese script:

- contiene la lógica de snapshots y nivelación,
- incluye un mapa estático de conexiones destino,
- puede incluir credenciales SSH y password de `sudo`,
- se registra en el scheduler nativo del sistema remoto.

Problemas:

- secretos dentro del propio script,
- permisos demasiado amplios,
- necesidad de reinstalar/actualizar payloads cuando cambian conexiones,
- validación SSH demasiado permisiva,
- paridad incompleta entre Unix/macOS y Windows.

## Principios del nuevo diseño

1. Separar código y datos
- El script GSA debe ser estable y no contener secretos.
- La configuración de conexiones y políticas debe ir en ficheros aparte.

2. Desplegar lo mínimo
- Cada conexión origen debe recibir solo los destinos realmente usados por sus datasets GSA.

3. Endurecer por defecto
- Permisos restrictivos.
- Validación de huellas SSH.
- Nada de `StrictHostKeyChecking=no`.

4. Mantener observabilidad
- Logs locales del GSA.
- Estado visible desde ZFSMgr.
- Versionado de script y configuración.

## Arquitectura propuesta

### Componentes desplegados en Unix/macOS

Script principal:

- `/usr/local/libexec/zfsmgr-gsa.sh`

Configuración general:

- `/etc/zfsmgr/gsa.conf`

Mapa de conexiones:

- `/etc/zfsmgr/gsa-connections.json`

Host keys conocidas:

- `/etc/zfsmgr/gsa_known_hosts`

Estado local opcional:

- `/var/lib/zfsmgr/gsa-state.json`

Log:

- `~/.config/ZFSMgr/GSA.log` para el usuario efectivo actual o
- `/var/log/zfsmgr-gsa.log` si se decide centralizarlo en fase posterior

### Componentes desplegados en Windows

Script principal:

- `C:\ProgramData\ZFSMgr\gsa.ps1`

Configuración general:

- `C:\ProgramData\ZFSMgr\gsa.json`

Mapa de conexiones:

- `C:\ProgramData\ZFSMgr\gsa-connections.json`

Known hosts / huellas:

- `C:\ProgramData\ZFSMgr\gsa-known-hosts`

Estado local opcional:

- `C:\ProgramData\ZFSMgr\gsa-state.json`

## Contenido de ficheros

### 1. Script principal

Responsabilidades:

- descubrir datasets `filesystem`,
- leer propiedades `org.fc16.gsa:*`,
- calcular clases vencidas,
- crear snapshots,
- podar retenciones,
- resolver destino usando la configuración,
- lanzar `send/recv`,
- escribir logs.

No debe contener:

- passwords,
- rutas de claves privadas de otras conexiones,
- mapas de conexiones incrustados,
- decisiones concretas de host key trust.

### 2. Configuración general

Ejemplo conceptual:

```json
{
  "schema_version": 1,
  "gsa_version": "0.9.9rc1-gsa1",
  "self_connection": "OrigenA",
  "os_type": "Linux",
  "log_file": "/root/.config/ZFSMgr/GSA.log",
  "known_hosts_file": "/etc/zfsmgr/gsa_known_hosts",
  "connections_file": "/etc/zfsmgr/gsa-connections.json"
}
```

Uso:

- desacoplar rutas y metadatos del script,
- permitir futuras migraciones de esquema.

### 3. Mapa mínimo de conexiones

Ejemplo conceptual:

```json
{
  "schema_version": 1,
  "generated_at": "2026-03-23T12:00:00Z",
  "source_connection": "OrigenA",
  "targets": {
    "Backup1": {
      "mode": "ssh",
      "host": "backup1.example.net",
      "port": 22,
      "user": "root",
      "use_sudo": false,
      "auth": {
        "type": "key",
        "key_path": "/root/.ssh/id_ed25519_zfsmgr"
      },
      "ssh_host_key": "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI..."
    },
    "Local": {
      "mode": "local"
    }
  }
}
```

Reglas:

- incluir solo destinos realmente referenciados por datasets GSA activos del origen,
- no incluir conexiones no usadas,
- no incluir metadatos de UI irrelevantes,
- no incluir conexiones desconectadas si no son necesarias para el origen.

### 4. Gestión de secretos

Diseño preferido:

- SSH por clave
- `sudo -n`

Diseño tolerado como transición:

- si hay password SSH o password de `sudo`, no debe vivir en el script principal;
- debe ir en fichero de configuración separado y protegido.

Opciones de almacenamiento:

1. Config protegida en disco
- más simple de implementar
- requiere permisos estrictos

2. Integración con almacén de secretos del SO
- macOS Keychain
- libsecret/gnome-keyring o equivalente
- Windows Credential Manager

Recomendación:

- Fase 1: config protegida en disco
- Fase 2: opcionalmente backend de secretos del sistema

## Permisos y ownership

### Unix/macOS

Directorio:

- `/etc/zfsmgr`
- permisos `700`
- propietario `root:wheel` o `root:root`

Ficheros:

- script `700` o `750`
- config general `600`
- mapa de conexiones `600`
- known_hosts `600`
- estado `600`

### Windows

`C:\ProgramData\ZFSMgr`

ACL:

- `SYSTEM`: full control
- `Administrators`: full control
- sin lectura para usuarios estándar salvo justificación expresa

## Scheduler

### Linux

- mantener `systemd service` + `systemd timer`

Cambios:

- el service debe ejecutar el script estable
- el script leerá la config externa

### macOS

- mantener `launchd`

Cambios:

- el plist ya no necesita regenerarse por cambios de conexiones salvo que cambie la ruta del script

### Windows

- mantener `Task Scheduler`

Cambios:

- la tarea invoca `gsa.ps1`
- el script PowerShell leerá `gsa.json` y `gsa-connections.json`

## Modelo de confianza SSH

### Estado actual a eliminar

No debe seguir usándose:

- `StrictHostKeyChecking=no`
- `UserKnownHostsFile=/dev/null`

### Nuevo modelo

Cada destino SSH debe tener:

- huella persistida en `gsa_known_hosts` o equivalente,
- validación estricta al conectar.

Durante instalación/actualización GSA:

- ZFSMgr recopila la huella esperada,
- la escribe en el `known_hosts` del GSA,
- si la huella cambia, la actualización debe:
  - avisar claramente,
  - requerir confirmación explícita,
  - dejar trazabilidad en log.

## Flujo de despliegue

### Instalación/actualización GSA

1. ZFSMgr analiza datasets de esa conexión.
2. Extrae destinos realmente usados.
3. Construye:
   - script estable,
   - config general,
   - mapa mínimo de conexiones,
   - fichero `known_hosts`.
4. Copia ficheros remotos.
5. Ajusta permisos.
6. Registra o actualiza scheduler nativo.
7. Refresca estado de conexión.

### Cambio de conexión en ZFSMgr

1. El usuario edita una conexión.
2. ZFSMgr identifica GSA instalados potencialmente afectados.
3. Regenera solo:
   - mapa de conexiones,
   - known_hosts,
   - config general si aplica.
4. No hace falta reemplazar el script salvo cambio de versión de lógica.

## Detección de conexiones afectadas

Objetivo final:

- no refrescar todos los GSA instalados indiscriminadamente.

Criterio:

- una conexión origen está afectada si algún dataset GSA activo suyo referencia como `Destino` la conexión modificada.

Implementación mínima aceptable:

- seguir refrescando todos los GSA instalados hasta tener este filtrado fino.

Implementación objetivo:

- calcular dependencias por `org.fc16.gsa:destino`
- refrescar solo orígenes afectados

## Compatibilidad y migración

### Compatibilidad hacia atrás

Durante una transición, el instalador puede aceptar dos formatos:

1. formato antiguo
- script autosuficiente con mapa incrustado

2. formato nuevo
- script estable + config externa

Detección:

- versión del script desplegado
- presencia de ficheros externos

### Migración propuesta

Fase 1:

- al actualizar GSA, desplegar ya el formato nuevo

Fase 2:

- detectar instalaciones antiguas y ofrecer migración explícita

Fase 3:

- retirar soporte para payload antiguo

## Impacto en la UI

La UI debería mostrar, por conexión:

- versión de script GSA
- versión de config
- fecha de último despliegue
- número de destinos incluidos en el mapa
- estado de huellas SSH
- si usa credenciales no endurecidas

Etiquetas útiles:

- `GSA actualizado`
- `Configuración GSA obsoleta`
- `Huellas SSH pendientes`
- `Usa password embebido`

## Limitaciones conocidas y decisiones

### Windows

No se debe prometer aún paridad completa con Unix/macOS para leveling remoto arbitrario.

Opciones:

1. Implementar la misma resolución remota basada en config externa.
2. O documentar Windows como soporte parcial para GSA.

### Secretos

Mientras no exista backend de secretos del SO:

- seguirá habiendo secretos en disco si la conexión depende de password,
- pero al menos dejarán de estar embebidos en el script.

## Plan recomendado de implementación

### Fase A

- crear formato de config externa
- modificar payload Unix/macOS para leerlo
- desplegar ficheros con permisos restrictivos

### Fase B

- añadir `known_hosts` propio y validación estricta
- eliminar `StrictHostKeyChecking=no`

### Fase C

- reducir mapa a destinos realmente usados
- mejorar detección de conexiones afectadas

### Fase D

- revisar Windows
- opcionalmente integrar almacenes de secretos del SO

## Resultado esperado

Con este diseño:

- el script GSA deja de ser contenedor de secretos,
- las actualizaciones por cambios de conexión son más pequeñas y más seguras,
- la confianza SSH pasa a ser verificable,
- y el comportamiento del GSA queda más mantenible y defendible.
