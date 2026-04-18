# Estado actual de instalación de comandos auxiliares

## Objetivo de este documento

Dejar fijado qué parte de la funcionalidad de instalación de comandos auxiliares ya está implementada, qué decisiones se han tomado y qué queda pendiente.

Documentos relacionados:

- [diseno_tecnico_instalacion_comandos_auxiliares.md](/home/linarese/work/ZFSMgr/docs/diseno_tecnico_instalacion_comandos_auxiliares.md)
- [plan_implementacion_comandos_auxiliares.md](/home/linarese/work/ZFSMgr/docs/plan_implementacion_comandos_auxiliares.md)

## Situación actual

La funcionalidad ya existe en una primera versión usable.

### Ya implementado

1. detección refinada de plataforma en `refreshConnection(...)`
   - Linux con detalle de distro a partir de `/etc/os-release`
   - macOS
   - FreeBSD
   - Windows

2. metadatos nuevos por conexión en `ConnectionRuntimeState`
   - plataforma de instalación auxiliar
   - gestor de paquetes esperado
   - si el gestor está detectado realmente
   - comandos instalables
   - comandos no soportados
   - paquetes resultantes
   - preview del comando remoto
   - motivo si no hay instalación asistida disponible

3. catálogo centralizado de instalación
   - fichero nuevo:
     - `src/helperinstallcatalog.*`
   - resuelve comandos faltantes a paquetes por plataforma

4. acción nueva en el menú contextual de `Conexiones`
   - `Instalar comandos auxiliares`

5. diálogo de preview antes de ejecutar
   - muestra:
     - conexión
     - plataforma
     - gestor de paquetes
     - comandos faltantes
     - comandos instalables
     - paquetes
     - comando remoto previsto
     - refresh posterior opcional

6. ejecución remota real en Unix
   - Linux
   - macOS
   - FreeBSD

7. integración correcta con `sudo`
   - la ejecución final pasa por `withSudo(...)`
   - no depende de un `sudo` incrustado en el preview
   - en FreeBSD ya quedó corregido el fallo de `sudo` sin TTY

8. refresh posterior y actualización del estado de conexión
   - tras instalar, la conexión se refresca
   - el tooltip vuelve a reflejar correctamente si el gestor está detectado

9. Windows integrado de forma básica
   - la misma acción `Instalar comandos auxiliares` deriva al flujo existente de `Instalar MSYS2`
   - no hay aún un plan explícito de paquetes Windows al mismo nivel que Unix

## Plataformas con estado actual

### Ubuntu / Debian

Estado:

- soportado por diseño
- plan de paquetes resuelto por `apt`
- ejecución remota implementada

### Arch Linux

Estado:

- soportado por diseño
- plan de paquetes resuelto por `pacman`
- ejecución remota implementada

### openSUSE / SUSE

Estado:

- soportado por diseño
- plan de paquetes resuelto por `zypper`
- ejecución remota implementada

### macOS

Estado:

- soportado por diseño
- plan de paquetes resuelto por `brew`
- ejecución remota implementada

Límite actual:

- si `brew` no está instalado, la instalación asistida no se puede ejecutar
- no se hace bootstrap automático de Homebrew

### FreeBSD

Estado:

- soportado por diseño
- plan de paquetes resuelto por `pkg`
- ejecución remota implementada
- validado al menos en el caso de uso ya probado con `pkg`

Corrección aplicada:

- el tooltip ahora sigue mostrando `pkg` como detectado incluso cuando ya no quedan comandos faltantes

### Windows

Estado:

- integrado funcionalmente en el mismo menú
- la acción deriva al flujo existente de MSYS2

Límite actual:

- no existe todavía un `HelperInstallPlan` Windows tan explícito como en Unix
- no se ha unificado aún la instalación de paquetes MSYS2 faltantes dentro del catálogo nuevo
- `winget` sigue siendo una ampliación futura, no una ruta nueva consolidada en esta funcionalidad

## Comandos actualmente contemplados por el catálogo

- `sshpass`
- `rsync`
- `pv`
- `mbuffer`
- `tar`
- `gzip`
- `zstd`
- `grep`
- `sed`
- `gawk`

## Decisiones tomadas

1. no instalar automáticamente sin confirmación del usuario
2. mostrar siempre preview del comando antes de ejecutar
3. no hacer bootstrap automático de Homebrew en esta fase
4. no intentar soportar distros Linux ambiguas sin detección fiable
5. reutilizar en Windows el flujo actual de `MSYS2` en lugar de inventar ya una segunda ruta paralela
6. reutilizar `withSudo(...)` para la ejecución real y no fiarse del preview textual

## Limitaciones pendientes

### 1. Windows todavía no está completamente absorbido por el nuevo modelo

Hoy Windows funciona, pero por derivación al flujo antiguo de `MSYS2`.

Falta:

- construir un plan explícito de paquetes MSYS2 faltantes desde el catálogo nuevo
- decidir si `winget` se integra o no como segunda capa real

### 2. Falta mejorar la granularidad del catálogo

Todavía hay decisiones conservadoras o simplificadas:

- `brew` usa fórmulas predefinidas, pero no se ha endurecido la detección de taps alternativos
- FreeBSD evita forzar `gtar`/`gsed`, pero no hay comprobación fina de si realmente hacen falta

### 3. Faltan tests específicos

No se han añadido aún tests automáticos dedicados a:

- construcción del plan por plataforma
- render del diálogo de preview
- ejecución asistida con mocks o stubs

### 4. Integración parcial con otras áreas

La integración con:

- `Conectividad`
- GSA

está reflejada en tooltip y en el modelo, pero no hay aún acciones rápidas contextuales del tipo:

- instalar `sshpass` directamente desde una celda roja de conectividad

## Próximo paso recomendado

El siguiente paso correcto no es abrir más UI, sino cerrar la base técnica.

Orden recomendado:

1. unificar Windows/MSYS2 con un plan explícito dentro del catálogo nuevo
2. añadir tests de construcción de plan por plataforma
3. decidir si merece la pena exponer `Copiar comando` en el diálogo
4. después, mejorar integración con `Conectividad` y GSA

## Commit de referencia

La funcionalidad quedó introducida en dos tandas:

- base de infraestructura y flujo inicial
- ajustes posteriores de `sudo`, tooltip y derivación Windows a MSYS2

Este documento existe para que la siguiente iteración pueda arrancar sin tener que reconstruir el contexto desde los diffs.
