# Automatización de release en GitHub

## Objetivo

Automatizar el flujo de publicación de una release de ZFSMgr desde una sola orden.

Script principal:

- [scripts/release-github.sh](/home/linarese/work/ZFSMgr/scripts/release-github.sh)

## Qué hace

Dado un parámetro de versión, por ejemplo:

```bash
./scripts/release-github.sh 0.10.1rc1
```

o en modo simulación:

```bash
./scripts/release-github.sh --dry-run 0.10.1rc1
```

el script:

1. valida que el árbol git esté limpio
2. actualiza la versión en `resources/CMakeLists.txt`
3. crea un commit `Release <version>`
4. hace `push` del commit al remoto git configurado
5. ejecuta `buildall.sh`
6. localiza los artefactos generados:
   - `ZFSMgr-<version>.app.zip`
   - `ZFSMgr-Setup-<version>*.exe`
   - `ZFSMgr-<version>-*.AppImage`
   - `zfsmgr_<version>_*.deb`
7. crea el tag `v<version>`
8. hace `push` del tag
9. crea la release en GitHub con nombre igual al tag
10. sube los cuatro artefactos a la release
11. guarda logs por fase de:
   - `git push`
   - `buildall.sh`
   - `git push` del tag
   - creación de la release en GitHub

## Supuestos

- el remoto git de publicación existe y por defecto se llama `github`
- `gh` está instalado y autenticado
- `buildall.sh` funciona correctamente en el entorno actual
- las máquinas remotas de Linux, macOS y Windows están preparadas para compilar
- los repositorios remotos que usa `buildall.sh` hacen `git pull --ff-only` sobre la rama correcta

## Variables útiles

- `GIT_REMOTE`
  - remoto git para `push` y tag
- `ARTIFACTS_DIR`
  - directorio base temporal para artefactos
- `OUTPUT_DIR`
  - directorio final pasado a `buildall.sh`
- `LINUX_REMOTE`
- `MAC_REMOTE`
- `WINDOWS_REMOTE`
- `RELEASE_LOG_DIR`
  - directorio donde guardar logs por fase

## Limitación importante

El commit de versión y su `push` ocurren antes de construir los artefactos.

Motivo:

- `buildall.sh` compila en remoto y esas máquinas actualizan el repo con `git pull --ff-only`
- si la nueva versión no está ya publicada en git, los remotos no pueden construirla

Consecuencia:

- si el build falla a mitad, el commit de versión ya existe y ya está subido
- la release y el tag no se crean hasta que el build termina bien

## Reentrada

El script ya tolera el caso en que la versión objetivo ya esté aplicada en `resources/CMakeLists.txt`.

En ese caso:

- no intenta crear un commit vacío
- hace `push` del `HEAD` actual
- continúa con build, tag y release

Esto permite retomar una release interrumpida antes de crear tag o release en GitHub.

## Logs

Por defecto, los logs se guardan en:

```text
.release-artifacts/logs/<version>/
```

Ficheros principales:

- `git-push.log`
- `buildall.log`
- `git-push-tag.log`
- `github-release.log`

## Dry Run

`--dry-run` valida:

- formato de versión
- árbol git limpio
- presencia de `gh`
- autenticación de `gh`
- inexistencia previa de tag y release

y después muestra el plan sin modificar versión, sin hacer commit y sin lanzar builds.

## Requisito operativo

Antes de ejecutar el script, el árbol debe estar limpio:

- sin cambios tracked pendientes
- sin cambios staged
- sin ficheros sin versionar

## Siguiente mejora posible

Si en el futuro se quiere hacer el flujo más robusto, el siguiente paso sería desacoplar `buildall.sh` de `git pull` remoto y permitirle construir un commit exacto por hash.
