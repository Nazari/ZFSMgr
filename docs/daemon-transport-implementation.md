---
id: daemon-transport-implementation
title: Implementación libssh del transport del daemon
---

## Objetivo

Reemplazar la capa TCP sintética de `DaemonTransport` por una implementación real sobre `libssh` que autentica con clave Ed25519 y permite gestionar el canal RPC JSON descrito en `docs/daemon-transport-protocol.md`.

## Dependencias

- `libssh` (>= 0.9) disponible en Linux, macOS y Windows. En macOS se puede empaquetar con Homebrew. En Windows el installer incluirá `libssh.dll` dentro del bundle.
- `libzfs_core` para los hosts Unix; Windows usa `zfs.exe`.

## Módulos

1. **`src/daemon_rpc_protocol.h`**: contrato compartido entre cliente y daemon. Define `RpcRequest`, `RpcResponse`, `RpcResult`, `PoolInfoJson`, `DSInfoJson` y `GsaPlanJson`.
2. **`DaemonTransport::probeDaemon`**: abre sesión `ssh_new()` + `ssh_options_set` (host, port 32099, user configurable), autentica con la clave privada generada y llama a `/health`.
3. **`DaemonTransport::call`**: abre canal, serializa `RpcRequest` a JSON compacto, lee `RpcResponse` desde el stream y valida `id`, `result` y `error`.

## Estado actual del daemon

El daemon ya deja de responder con estructuras hardcoded en `/pools`, `/datasets` y `/datasets/properties`.

- `/pools` y `/datasets` usan `libzfs` cargado dinámicamente para enumerar pools importados y datasets/snapshots del pool solicitado.
- `/pools/details` devuelve propiedades y `zpool status` del pool solicitado; el cliente lo usa para rellenar la información del panel de pool sin depender de `ssh`.
- `/permissions` devuelve el volcado estructurado de `zfs allow` más listas de usuarios/grupos del sistema.
- `/holds` devuelve los holds activos del dataset/snapshot solicitado.
- `/datasets/properties` abre el dataset real con `libzfs` y devuelve sus propiedades efectivas en JSON.
- `/gsa` todavía combina la base `libzfs` con una consulta auxiliar de propiedades `org.fc16.gsa:*` para reconstruir el plan efectivo por dataset.
- Windows sigue marcado como backend pendiente: el contrato existe, pero el enumerador real aún queda para el proxy específico de esa plataforma.

## Firma Ed25519 y headers

Cada request se asocia con el fingerprint de la clave pública permitida por el daemon. La firma del payload y los headers `X-Key-Fingerprint`/`X-Signature` siguen siendo una extensión prevista para la siguiente iteración del daemon.

## Reintentos y timeouts

- `probeDaemon` usa timeout de 2s; si falla marca `isDaemonAvailable=false` y planifica reintentos cada 30s.
- `call` usa un timeout de 5s y captura errores de canal para forzar fallback.

## Logging y tracing

El transport debe trazar: método, host, fingerprint, duración y error (HTTP-like). Los logs se integran con `appendConnectionLog`.

## Pruebas

- Crear un stub local de libssh que reemplace la sesión con un proceso simulado y responde a `/health` y `/exec` (para test unitario). La prueba valida que el client reintente y caiga a SSH cuando se simula error 601.
