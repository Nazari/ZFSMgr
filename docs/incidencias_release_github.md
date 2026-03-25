# Incidencias reales de la automatización de release

## Objetivo

Dejar constancia de los problemas reales encontrados al ejecutar una release completa y de las decisiones tomadas para que el flujo sea retomable.

## Release usada para validación

- versión: `0.10.1rc1`
- tag: `v0.10.1rc1`

## Incidencias detectadas y correcciones

### 1. `gh` no estaba instalado en el host lanzador

Síntoma:

```text
Error: No se encontró el comando requerido: gh
```

Decisión:

- mantener el preflight estricto
- fallar antes de tocar versión, commits o tags

Estado:

- resuelto operativamente instalando `gh`

### 2. Los builders podían reutilizar una versión vieja desde cachés de build

Síntoma:

- `build-macos.sh` llegó a esperar `ZFSMgr-0.10.0rc1.app`
- el release ya estaba intentando publicar `0.10.1rc1`

Causa:

- los scripts de build seguían pudiendo preferir información obsoleta del `CMakeCache.txt`

Decisión:

- la versión se resuelve primero desde `resources/CMakeLists.txt`
- el cache solo puede ser fallback

Estado:

- corregido en `scripts/build-macos.sh` y `scripts/build-linux.sh`

### 3. La reejecución del script fallaba si la versión ya estaba aplicada

Síntoma:

```text
nothing to commit, working tree clean
```

Causa:

- el script asumía siempre que habría un commit nuevo

Decisión:

- si no hay diff staged tras actualizar la versión, continuar sin crear commit nuevo

Estado:

- corregido en `scripts/release-github.sh`

### 4. La firma de macOS no era apta para automatización no interactiva

Síntoma:

```text
security: ... User interaction is not allowed
errSecInternalComponent
```

Causa:

- el builder remoto intentaba firmar el `.app` con acceso al keychain

Decisión:

- el pipeline automatizado genera el artefacto macOS sin firma
- `buildall.sh` usa `./scripts/build-macos.sh --bundle --no-sign`

Estado:

- corregido

Consecuencia:

- el `app.zip` de release no está firmado dentro de este pipeline

### 5. La copia del `.app` remoto en macOS no podía hacerse con `scp` directo

Síntoma:

```text
scp: ... not a regular file
```

Causa:

- el artefacto macOS es un directorio `.app`

Decisión:

- mantener un fallback por `tar` vía `ssh`

Estado:

- validado en ejecución real

### 6. El build de Windows es poco observable en tiempo real

Síntoma:

- tramos largos sin salida
- solo aparecían warnings intermedios puntuales

Decisión:

- añadir logs por fase en `release-github.sh`
- dejar para más adelante una mejora de observabilidad más fina dentro de `buildall.sh`

Estado:

- parcialmente mitigado con logs por fase

## Decisiones de diseño actuales

- el commit de versión se hace antes del build
- los builders remotos ya no construyen “la rama actual”, sino el `commit` exacto que les pasa `release-github.sh`
- el script permite reentrada si la versión ya estaba aplicada
- el script ofrece también `--resume` explícito
- los logs por fase se guardan en `.release-artifacts/logs/<version>/`
- el bundle de macOS de release no se firma en este pipeline

## Pendientes razonables

- desacoplar `buildall.sh` de `git pull` remoto y construir por hash exacto
- añadir `--resume` explícito si en el futuro se necesita distinguirlo de la reentrada implícita
- mejorar logs internos por plataforma dentro de `buildall.sh`
- decidir si el flujo de release final debe soportar firma macOS opcional en un entorno CI preparado
