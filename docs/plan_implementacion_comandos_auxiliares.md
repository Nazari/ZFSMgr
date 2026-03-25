# Plan de implementación de instalación de comandos auxiliares

## Objetivo

Convertir el diseño de instalación de comandos auxiliares en una implementación incremental, con riesgo controlado y validación por plataforma.

Documento base relacionado:

- [diseno_tecnico_instalacion_comandos_auxiliares.md](/home/linarese/work/ZFSMgr/docs/diseno_tecnico_instalacion_comandos_auxiliares.md)

## Estrategia general

La implementación debe avanzar por capas:

1. detección fiable de plataforma y gestor de paquetes,
2. catálogo centralizado de comandos y paquetes,
3. cálculo de un plan de instalación por conexión,
4. UI mínima para lanzar la instalación,
5. ejecución remota y verificación,
6. ampliación a Windows.

La prioridad correcta no es Windows, sino Unix primero:

- Linux
- macOS
- FreeBSD

Motivo:

- el canal remoto ya está resuelto por `SSH`
- ya existe detección de comandos faltantes
- el modelo de privilegios es más homogéneo
- el riesgo de romper flujos existentes es menor que en Windows

## Fase 0: saneamiento previo

## Objetivo

Preparar la base para no repartir lógica por varios ficheros.

## Tareas

1. localizar la detección actual de comandos faltantes y consolidar el conjunto de comandos observados realmente
2. inventariar qué comandos faltantes ya provocan estado naranja en `Conexiones`
3. separar claramente:
   - comandos faltantes detectados
   - comandos faltantes instalables
   - comandos faltantes no soportados por la plataforma
4. revisar Windows/MSYS2 existente para no duplicar flujos ya implementados

## Salida esperada

- lista cerrada de comandos auxiliares soportados en la primera versión
- lista de código existente reutilizable

## Riesgo

- bajo

## Fase 1: detección de plataforma y gestor de paquetes

## Objetivo

Disponer de una clasificación robusta por conexión.

## Alcance

- Ubuntu
- Debian
- Arch
- openSUSE / SUSE
- macOS con `brew`
- FreeBSD con `pkg`

## Tareas

1. ampliar `ConnectionRuntimeState` con metadatos de instalación auxiliar
2. detectar distro Linux mediante `/etc/os-release`
3. detectar gestor de paquetes disponible:
   - `apt-get`
   - `pacman`
   - `zypper`
   - `brew`
   - `pkg`
4. determinar si la conexión es candidata a instalación asistida
5. dejar reflejado en tooltip de conexión:
   - plataforma
   - gestor detectado
   - si la instalación asistida está soportada

## Salida esperada

- `helperInstallSupported`
- `packageManagerId`
- `missingHelperCommands`

## Riesgo

- bajo

## Fase 2: catálogo centralizado

## Objetivo

Mover a un solo sitio la traducción de comandos faltantes a paquetes y comandos de instalación.

## Tareas

1. crear un catálogo central, por ejemplo conceptual:
   - `helperinstallcatalog.*`
2. definir una estructura de datos por plataforma con:
   - comando lógico
   - paquete o paquetes
   - verificación posterior
   - observaciones
3. cubrir como mínimo:
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
4. modelar excepciones:
   - macOS `sshpass`
   - FreeBSD `gtar`/`gsed` solo si realmente se necesitan

## Salida esperada

- API que reciba plataforma + comandos faltantes y devuelva paquetes soportados

## Riesgo

- medio

## Fase 3: plan de instalación por conexión

## Objetivo

Construir una representación explícita del trabajo a ejecutar antes de tocar la UI.

## Tareas

1. crear un objeto tipo `HelperInstallPlan`
2. incluir en el plan:
   - conexión
   - plataforma
   - gestor de paquetes
   - comandos faltantes soportados
   - comandos faltantes no soportados
   - paquetes finales a instalar
   - comando remoto final
   - advertencias
3. resolver instalaciones incrementales:
   - solo paquetes necesarios
4. bloquear plan si:
   - distro no reconocida
   - falta gestor de paquetes
   - falta `sudo` en Unix cuando es imprescindible
   - macOS sin `brew`

## Salida esperada

- función `buildHelperInstallPlan(connIdx)` o equivalente

## Riesgo

- medio

## Fase 4: UI mínima en Conexiones

## Objetivo

Exponer la funcionalidad con el menor cambio posible en la interfaz.

## Tareas

1. añadir acción contextual:
   - `Instalar comandos auxiliares...`
2. habilitarla solo si:
   - la conexión está en `OK con comandos faltantes`
   - la plataforma es soportada
3. mostrar tooltip si está deshabilitada
4. crear diálogo simple con:
   - conexión
   - plataforma/gestor
   - comandos faltantes
   - paquetes a instalar
   - comando exacto
   - advertencias
   - check `Refrescar conexión al terminar`

## Criterio de diseño

- no inventar una ventana compleja
- priorizar claridad y auditabilidad

## Riesgo

- bajo

## Fase 5: ejecución remota y verificación

## Objetivo

Ejecutar la instalación y cerrar el ciclo con refresh real.

## Tareas

1. ejecutar el comando remoto usando las primitivas actuales
2. registrar en logs:
   - comando lanzado
   - salida resumida
   - código de salida
3. si termina bien:
   - volver a detectar comandos faltantes
   - refrescar conexión
4. si termina mal:
   - mostrar error concreto
   - no cambiar falsamente el estado visual

## Criterio de aceptación

- la fila cambia de naranja a verde cuando la instalación resuelve el problema
- o permanece naranja con menor número de comandos faltantes

## Riesgo

- medio

## Fase 6: integración con Conectividad y GSA

## Objetivo

Cerrar el circuito con los dos consumidores principales de comandos auxiliares.

## Tareas

1. si faltan `sshpass` o `rsync`, enlazar mejor con la matriz de `Conectividad`
2. mejorar tooltips de conexión para distinguir:
   - faltan comandos de conectividad
   - faltan comandos de GSA
   - faltan comandos genéricos de copia
3. si una instalación resuelve `sshpass` o `rsync`, refrescar también la información relacionada si procede

## Riesgo

- medio

## Fase 7: Windows

## Objetivo

Integrar la instalación asistida en Windows sin abrir deuda estructural innecesaria.

## Enfoque recomendado

No mezclar desde el primer día:

- instalación de MSYS2
- instalación de paquetes MSYS2
- instalación por `winget`

Orden correcto:

1. reutilizar flujo actual de MSYS2
2. construir plan de paquetes MSYS2 faltantes
3. verificar comandos tras instalar
4. dejar `winget` para una fase posterior solo si hace falta

## Tareas

1. detectar si MSYS2 existe y es usable
2. si no existe:
   - reutilizar acción existente o integrarla en el plan
3. si existe:
   - instalar paquetes MSYS2 faltantes
4. mapear comandos faltantes a paquetes MSYS2
5. verificar que la conexión pasa a estado correcto

## Riesgo

- alto

## Orden recomendado de entrega

### Entrega A

- Fase 0
- Fase 1
- Fase 2
- Fase 3

Resultado:

- infraestructura lista sin UI visible todavía

### Entrega B

- Fase 4
- Fase 5

Resultado:

- instalación asistida funcional en Linux, macOS y FreeBSD
- Windows entra de forma básica reutilizando el flujo actual de MSYS2

### Entrega C

- Fase 6

Resultado:

- mejor integración funcional con conectividad y GSA

### Entrega D

- Fase 7

Resultado:

- refinamiento del soporte Windows: plan explícito de paquetes MSYS2 y, si compensa, integración más rica con `winget`

## Pruebas mínimas por plataforma

## Ubuntu / Debian

Casos:

1. falta `sshpass`
2. faltan `sshpass` y `rsync`
3. faltan `pv` y `mbuffer`
4. sin `sudo` configurado

## Arch

Casos:

1. falta `rsync`
2. faltan varias utilidades auxiliares
3. `pacman` accesible pero instalación falla

## openSUSE / SUSE

Casos:

1. falta `sshpass`
2. `zypper` presente
3. salida de error manejada correctamente

## macOS

Casos:

1. `brew` presente
2. `brew` ausente
3. falta `sshpass` y requiere tap

## FreeBSD

Casos:

1. `pkg` presente
2. faltan `rsync`, `pv`, `mbuffer`, `zstd`, `gawk`
3. comprobar que no se fuerza `gtar` o `gsed` si no hacen falta

## Windows

Casos:

1. falta MSYS2
2. MSYS2 presente pero faltan paquetes
3. verificación correcta tras la instalación

## Decisiones prácticas

- empezar por Unix y no por Windows
- no instalar Homebrew automáticamente en la primera versión
- no actualizar el sistema completo
- no instalar herramientas no detectadas como necesarias
- usar siempre preview del comando antes de ejecutar
- registrar todo en logs

## Próximo paso recomendado

Empezar por las fases 0 a 3 en código, sin tocar todavía la UI más allá de un posible stub interno para inspeccionar el plan generado.
