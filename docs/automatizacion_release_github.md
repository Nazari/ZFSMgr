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
./scripts/release-github.sh --resume 0.10.1rc1
./scripts/release-github.sh --resume --skip-build 0.10.1rc1
./scripts/release-github.sh --only-release 0.10.1rc1
./scripts/release-github.sh --resume --platforms linux,windows 0.10.2rc1
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
12. guarda un manifiesto de estado por versión en JSON

Durante `buildall.sh`, los builders remotos reciben además el `commit` exacto a construir.
Ya no dependen de la rama activa remota ni de que `git pull` los deje en el estado esperado.

## Supuestos

- el remoto git de publicación existe y por defecto se llama `github`
- `gh` está instalado y autenticado
- `buildall.sh` funciona correctamente en el entorno actual
- las máquinas remotas de Linux, macOS y Windows están preparadas para compilar
- los repositorios remotos que usa `buildall.sh` tienen acceso al remoto git indicado y pueden hacer `fetch` del commit objetivo

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
- `BUILD_PLATFORMS`
  - lista separada por comas para `buildall.sh`
  - valores admitidos: `mac`, `linux`, `windows`

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

## Resume explícito

`--resume` sirve para retomar una release cuando:

- la versión ya quedó aplicada en `resources/CMakeLists.txt`
- quieres evitar tocar otra vez la versión
- quieres forzar que el flujo continúe desde el `HEAD` actual
- quieres reutilizar artefactos ya construidos si siguen presentes
- quieres reutilizar un tag ya creado si la release final todavía no existe

Ejemplo:

```bash
./scripts/release-github.sh --resume 0.10.1rc1
```

Condición:

- la versión actual del código debe coincidir ya con la versión pedida

No hace:

- ni rollback
- ni limpieza de tags o releases ya creadas

Sí hace:

- recuperar el tag local desde el remoto si existe solo en GitHub
- saltarse `buildall.sh` si los cuatro artefactos ya están presentes
- saltarse la creación o el push del tag si ese paso ya estaba completado

## Skip Build

`--skip-build` sirve para reutilizar artefactos ya presentes en:

```text
.release-artifacts/<version>/
```

Condición:

- los cuatro artefactos deben existir ya

Si no existen, el script falla al validar artefactos, que es el comportamiento correcto.

## Plataformas selectivas

Puedes limitar `buildall.sh` a plataformas concretas:

```bash
./scripts/release-github.sh --resume --platforms linux,windows 0.10.2rc1
```

Valores admitidos:

- `mac`
- `linux`
- `windows`

Uso típico:

- relanzar solo Windows tras un fallo del instalador
- regenerar solo Linux cuando cambió AppImage o DEB

## Only Release

`--only-release` equivale a:

- `--resume`
- `--skip-build`

y además exige que el tag ya exista.

Caso de uso:

- build ya completado
- tag ya creado y publicado
- release de GitHub pendiente de crear o de reintentar

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

Logs por plataforma de `buildall.sh`:

- `.release-artifacts/logs/<version>/buildall-platforms/macos-local.log`
- `.release-artifacts/logs/<version>/buildall-platforms/macos-remote.log`
- `.release-artifacts/logs/<version>/buildall-platforms/linux-remote.log`
- `.release-artifacts/logs/<version>/buildall-platforms/windows-remote.log`

Estado persistente:

- `.release-artifacts/state/<version>/release-state.json`

Dentro de `buildall.log` queda también trazado el `BUILD_GIT_REF` exacto usado por los builders remotos.
El `release-state.json` guarda además el inventario de artefactos detectados.

## Dry Run

`--dry-run` valida:

- formato de versión
- árbol git limpio
- presencia de `gh`
- autenticación de `gh`
- inexistencia previa de tag y release

y después muestra el plan sin modificar versión, sin hacer commit y sin lanzar builds.

En `dry-run` también informa si:

- el tag ya existe local o remotamente
- la release ya existe en GitHub

## Requisito operativo

Antes de ejecutar el script, el árbol debe estar limpio:

- sin cambios tracked pendientes
- sin cambios staged
- sin ficheros sin versionar

## Siguiente mejora posible

Si en el futuro se quiere hacer el flujo más robusto, el siguiente paso sería:

- persistir un manifiesto de release con estado por fase
- añadir limpieza selectiva de artefactos o relanzamiento por plataforma
