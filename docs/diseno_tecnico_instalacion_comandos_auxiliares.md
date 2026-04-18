# Diseño técnico de instalación de comandos auxiliares

## Objetivo

Permitir que ZFSMgr instale desde la propia aplicación los comandos auxiliares que falten en una conexión ya operativa.

El objetivo no es instalar ZFS ni preparar una máquina desde cero, sino cubrir dependencias auxiliares que ZFSMgr y el GSA usan para:

- conectividad entre conexiones,
- transferencias y copias,
- nivelaciones GSA,
- compresión y empaquetado,
- utilidades Unix básicas en Windows.

## Motivación

Hoy ZFSMgr ya detecta conexiones en estado:

- `OK` con comandos faltantes

Ese estado es útil para diagnóstico, pero obliga al usuario a salir de ZFSMgr para resolverlo manualmente.

La funcionalidad propuesta añade un flujo guiado para:

1. detectar qué comandos faltan,
2. traducirlos a paquetes instalables según el sistema,
3. ejecutar la instalación remota desde ZFSMgr,
4. verificar el resultado,
5. refrescar el estado de la conexión.

## Alcance

### Sistemas soportados

La primera versión debe cubrir:

- Windows
- FreeBSD
- macOS
- Linux Ubuntu
- Linux Debian
- Linux Arch
- Linux openSUSE / SUSE

### Fuera de alcance en la primera versión

- RHEL / CentOS / Fedora
- Alpine
- NixOS
- instalación de OpenZFS
- bootstrap completo de SSH o `sudo`
- instalación silenciosa automática sin confirmación del usuario

## Comandos objetivo

ZFSMgr no necesita el mismo conjunto de comandos en todas las conexiones ni para todas las operaciones. Conviene distinguir entre:

### Comandos críticos de conectividad

- `sshpass`
- `rsync`

### Comandos de transferencia y copia

- `pv`
- `mbuffer`
- `tar`
- `gzip`
- `zstd`

### Utilidades Unix básicas

- `grep`
- `sed`
- `gawk`
- `awk` si la plataforma lo separa de `gawk`

### Windows

En Windows hay que distinguir dos capas:

1. herramientas nativas o instalables por `winget`
2. herramientas Unix proporcionadas por `MSYS2`

En la práctica, para Windows el objetivo principal es asegurar una base MSYS2 suficiente para que existan:

- `tar`
- `gzip`
- `zstd`
- `rsync`
- `grep`
- `sed`
- `gawk`

## Modelo de detección

ZFSMgr ya calcula comandos faltantes en runtime. Esta funcionalidad debe reutilizar esa detección, no duplicarla.

### Estado base

Por conexión, el runtime ya debería poder responder:

- sistema operativo detectado,
- familia o distribución,
- comandos auxiliares disponibles,
- comandos auxiliares ausentes,
- si existe un instalador soportado.

### Nuevos metadatos propuestos

Ampliar `ConnectionRuntimeState` con campos conceptuales equivalentes a:

- `packageManagerId`
- `packageManagerDetected`
- `helperInstallSupported`
- `missingHelperCommands`
- `helperInstallPlanSummary`

### Detección de sistema y gestor de paquetes

#### Linux

Primero detectar distribución. Prioridad recomendada:

1. `/etc/os-release`
2. comandos nativos de la distro si hiciera falta

Mapeo inicial:

- Ubuntu -> `apt`
- Debian -> `apt`
- Arch -> `pacman`
- openSUSE / SUSE -> `zypper`

#### macOS

Usar `brew` si está instalado.

Si `brew` no existe:

- no intentar instalar Homebrew automáticamente en la primera versión
- informar que falta `brew` y no hay instalación automática disponible todavía

#### FreeBSD

Usar `pkg`.

#### Windows

Jerarquía propuesta:

1. si falta base Unix, usar flujo MSYS2
2. si faltan herramientas nativas no cubiertas por MSYS2, usar `winget` cuando aplique

## Mapeo de comandos a paquetes

El sistema debe trabajar con un catálogo declarativo por plataforma, no con lógica dispersa en el código.

## Formato conceptual del catálogo

Cada entrada debería definir:

- comando lógico: `rsync`
- plataformas soportadas
- paquete o paquetes a instalar
- comando de verificación postinstalación
- si requiere `sudo` o elevación
- notas de seguridad o limitaciones

## Mapeo inicial recomendado

### Ubuntu / Debian

- `sshpass` -> `sshpass`
- `rsync` -> `rsync`
- `pv` -> `pv`
- `mbuffer` -> `mbuffer`
- `tar` -> `tar`
- `gzip` -> `gzip`
- `zstd` -> `zstd`
- `grep` -> `grep`
- `sed` -> `sed`
- `gawk` -> `gawk`

Instalación agrupada recomendada:

```bash
sudo apt-get update && sudo apt-get install -y sshpass rsync pv mbuffer tar gzip zstd grep sed gawk
```

En implementación real conviene instalar solo los paquetes realmente necesarios.

### Arch Linux

- `sshpass` -> `sshpass`
- `rsync` -> `rsync`
- `pv` -> `pv`
- `mbuffer` -> `mbuffer`
- `tar` -> `tar`
- `gzip` -> `gzip`
- `zstd` -> `zstd`
- `grep` -> `grep`
- `sed` -> `sed`
- `gawk` -> `gawk`

Comando base:

```bash
sudo pacman -Sy --noconfirm sshpass rsync pv mbuffer tar gzip zstd grep sed gawk
```

### openSUSE / SUSE

- `sshpass` -> `sshpass`
- `rsync` -> `rsync`
- `pv` -> `pv`
- `mbuffer` -> `mbuffer`
- `tar` -> `tar`
- `gzip` -> `gzip`
- `zstd` -> `zstd`
- `grep` -> `grep`
- `sed` -> `sed`
- `gawk` -> `gawk`

Comando base:

```bash
sudo zypper --non-interactive install sshpass rsync pv mbuffer tar gzip zstd grep sed gawk
```

### macOS

- `sshpass` -> `hudochenkov/sshpass/sshpass` o fórmula equivalente disponible
- `rsync` -> `rsync`
- `pv` -> `pv`
- `mbuffer` -> `mbuffer`
- `zstd` -> `zstd`
- `grep` -> `grep`
- `sed` -> `gnu-sed` si se requiere comportamiento GNU, si no `sed` del sistema
- `gawk` -> `gawk`

Observación:

- `tar` y `gzip` suelen existir ya en macOS.
- ZFSMgr debe evitar reinstalar herramientas ya presentes.
- `sshpass` en macOS puede requerir tap adicional. Esa parte debe modelarse explícitamente en el catálogo.

### FreeBSD

- `sshpass` -> `security/sshpass` o paquete `sshpass`
- `rsync` -> `rsync`
- `pv` -> `pv`
- `mbuffer` -> `mbuffer`
- `tar` -> `gtar` si se exige GNU tar; si no, usar el `tar` del sistema
- `gzip` -> `gzip`
- `zstd` -> `zstd`
- `grep` -> `grep`
- `sed` -> `gsed` si se exige GNU sed; si no, usar `sed` del sistema
- `gawk` -> `gawk`

Comando base:

```bash
sudo pkg install -y sshpass rsync pv mbuffer zstd grep gawk
```

Notas:

- FreeBSD requiere diferenciar claramente entre utilidades BSD válidas y utilidades GNU necesarias.
- No conviene imponer `gtar` o `gsed` si ZFSMgr realmente no las necesita.

### Windows

#### Ruta preferente

- asegurar `MSYS2`
- instalar los paquetes Unix dentro de MSYS2

Paquetes base ya coherentes con el código actual:

- `tar`
- `gzip`
- `zstd`
- `rsync`
- `grep`
- `sed`
- `gawk`

Extensión propuesta:

- `pv`
- `mbuffer`
- `sshpass` solo si está disponible y mantenible en MSYS2; si no, dejarlo como no soportado

#### Complemento nativo

Si en el futuro hicieran falta herramientas fuera de MSYS2:

- `winget install ...`

Pero en primera versión conviene concentrar la base auxiliar en MSYS2 para no abrir dos ecosistemas a la vez.

## Experiencia de usuario propuesta

## Estado visual

Mantener el estado naranja actual para:

- `OK con comandos faltantes`

Añadir además:

- tooltip con lista exacta de comandos faltantes,
- si la plataforma es soportada, texto adicional indicando que pueden instalarse desde ZFSMgr.

## Menú contextual en Conexiones

Añadir una acción específica:

- `Instalar comandos auxiliares...`

Comportamiento:

- visible o habilitada solo si la conexión está en `OK con comandos faltantes`
- deshabilitada si la plataforma no tiene instalador soportado
- si está deshabilitada, tooltip con motivo
- en Windows, esta misma acción reutiliza el flujo existente de `MSYS2` en lugar de abrir un instalador distinto

## Diálogo de instalación

Propuesta de contenido:

- conexión objetivo
- sistema detectado y gestor de paquetes detectado
- comandos faltantes
- paquetes que se van a instalar
- comando exacto que ZFSMgr ejecutará
- nota sobre privilegios necesarios
- checkbox opcional para `Refrescar conexión al terminar`

Botones:

- `Cancelar`
- `Instalar`

Opcional en una segunda fase:

- `Copiar comando`
- `Ver detalle`

## Modelo de ejecución

### Principio general

ZFSMgr debe ejecutar la instalación remotamente usando la misma conexión ya configurada.

No debe inventar un canal nuevo.

### Unix, macOS y FreeBSD

Ejecución mediante la ruta actual de `SSH` y `sudo`.

Reglas:

- si la conexión tiene `sudo`, usarlo
- si no tiene `sudo` y la instalación requiere privilegios, bloquear con mensaje claro
- usar modo no interactivo cuando sea posible
- registrar en logs el comando lanzado y su salida resumida

### Windows

La implementación actual reutiliza el flujo existente de `MSYS2` desde la misma acción `Instalar comandos auxiliares...`.

En la práctica:

- si faltan comandos auxiliares en Windows, ZFSMgr deriva a la preparación de `MSYS2`
- el despliegue instala o verifica `MSYS2` y completa el conjunto base soportado
- la verificación posterior sigue pasando por el refresh normal de la conexión

El soporte de `winget` fuera de este flujo queda como ampliación futura, no como ruta separada en la UI actual.

## Seguridad y límites

Esta funcionalidad ejecuta instalaciones de paquetes remotas. No debe ser automática ni silenciosa.

### Reglas de seguridad

- siempre debe haber confirmación explícita del usuario
- siempre debe mostrarse el comando a ejecutar antes de lanzar la instalación
- debe quedar traza en logs de aplicación y de ejecución remota
- no se deben concatenar instalaciones no relacionadas si la plataforma no está bien identificada
- si la detección de distro es ambigua, no instalar

### Reglas de acotación

- instalar solo lo que falta
- no actualizar el sistema completo
- no cambiar repositorios del sistema en la primera versión
- no instalar Homebrew automáticamente
- no instalar `sudo`, `ssh` o ZFS automáticamente

## Verificación posterior

Al terminar la instalación:

1. re-ejecutar verificación de comandos faltantes,
2. refrescar la conexión,
3. actualizar color/tooltip de la fila,
4. si procede, actualizar matrices o checks dependientes.

Resultado esperado:

- la conexión pasa de `OK con comandos faltantes` a `OK`
- o permanece en naranja con una lista menor de comandos faltantes

## Integración con otras áreas

## Conectividad

La matriz de `Conectividad` ya depende de `sshpass` y `rsync`.

Esta nueva funcionalidad debe enlazarse con ella de dos formas:

- si una celda roja se debe a falta de `sshpass` o `rsync`, la conexión origen debería poder resolverse desde `Conexiones`
- el tooltip de la conexión debe reflejar que faltan comandos auxiliares relevantes para conectividad

## GSA

El GSA también depende de utilidades auxiliares en algunas rutas:

- `sshpass`
- `zstd`
- `tar`
- `gzip`
- `grep`
- `sed`
- `gawk`

Cuando una conexión tenga GSA instalado pero le falten herramientas necesarias para nivelación o despliegue, esta funcionalidad debe ser reutilizable desde la misma fila de conexión.

## Arquitectura propuesta

## Catálogo centralizado

Crear un catálogo central de instalación de herramientas, por ejemplo conceptual:

- `HelperInstallCatalog`
- `HelperPackageResolver`

Debe mapear:

- plataforma
- distro
- comando faltante
- paquete(s)
- comando de instalación
- comando de verificación

La lógica no debe quedar repartida por `if` aislados en varios ficheros.

## Plan de instalación

Crear un objeto de plan por conexión, por ejemplo conceptual:

- `HelperInstallPlan`

Con campos como:

- conexión
- plataforma detectada
- gestor de paquetes
- comandos faltantes soportados
- comandos faltantes no soportados
- paquetes a instalar
- comando remoto final
- advertencias

Ese plan será la base tanto para el diálogo como para la ejecución.

## Ejecución

Reutilizar las primitivas ya existentes:

- ejecución remota por `SSH`
- ejecución PowerShell remota en Windows cuando proceda
- logging de comandos
- refresco posterior de la conexión

## Casos de fallo previstos

- distro no reconocida
- gestor de paquetes ausente
- conexión sin `sudo` cuando hace falta
- repositorio sin paquete equivalente
- `brew` no instalado en macOS
- `winget` no disponible en Windows
- `MSYS2` no instalado y su instalación falla
- instalación parcial: algunos comandos quedan resueltos y otros no

La UI debe reflejar estos casos con mensajes concretos, no solo con `Error instalando`.

## Fases de implementación recomendadas

### Fase 1

- catálogo para Ubuntu, Debian, Arch, openSUSE/SUSE, FreeBSD y macOS
- acción `Instalar comandos auxiliares...`
- diálogo con preview del comando
- ejecución remota
- verificación y refresh

### Fase 2

- integración mejorada con Windows/MSYS2
- detección más rica de `winget`
- soporte de paquetes opcionales adicionales

### Fase 3

- sugerencias contextuales desde la matriz de conectividad
- acciones rápidas cuando solo falta `sshpass` o `rsync`
- documentación de ayuda para el usuario final

## Criterio de aceptación

La funcionalidad se considerará aceptable cuando:

1. una conexión Ubuntu/Debian con `sshpass` y `rsync` ausentes pueda resolverse desde ZFSMgr,
2. una conexión macOS con `brew` disponible pueda instalar herramientas auxiliares soportadas,
3. una conexión FreeBSD pueda instalar al menos `rsync`, `pv`, `mbuffer`, `zstd` y `gawk`,
4. una conexión Windows con MSYS2 pueda instalar el conjunto base Unix soportado,
5. el estado de la fila cambie correctamente tras la verificación,
6. toda instalación deje trazas claras y revisables.

## Decisiones recomendadas

- Sí a una acción explícita `Instalar comandos auxiliares...`
- Sí a un catálogo centralizado por plataforma
- Sí a preview del comando antes de ejecutar
- Sí a instalación incremental solo de lo que falta
- No a instalación automática silenciosa
- No a soporte de distros ambiguas sin detección fiable
- No a bootstrap automático de Homebrew en la primera versión
