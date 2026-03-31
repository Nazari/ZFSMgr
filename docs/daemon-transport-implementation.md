---
id: daemon-transport-implementation
title: Implementación libssh del transport del daemon
---

## Objetivo

Reemplazar la capa TCP sintética de `DaemonTransport` por una implementación real sobre `libssh` que autentica con clave Ed25519, firma cada request y permite gestionar el canal RPC JSON descrito en `docs/daemon-transport-protocol.md`.

## Dependencias

- `libssh` (>= 0.9) disponible en Linux, macOS y Windows. En macOS se puede empaquetar con Homebrew. En Windows el installer incluirá `libssh.dll` dentro del bundle.
- `libzfs_core` para los hosts Unix; Windows usa `zfs.exe`.

## Módulos

1. **DaemonRpcMessage**: struct con `id`, `method`, `params`, `result`, `error`. Serializa/parsea JSON y valida la inclusión de `fallback`.
2. **DaemonTransport::probeDaemon**: abre sesión `ssh_new()` + `ssh_options_set` (host, port 32099, user configurable). Carga la clave pública generada (`ssh_privatekey_new`), se conecta con `ssh_connect()`, comprueba `ssh_userauth_publickey_auto`, envía `GET /health` y cierra la sesión.
3. **DaemonTransport::call**: reutiliza la sesión (`ssh_channel_new`, `ssh_channel_request_exec` o `ssh_channel_request_subsystem`), envía la petición JSON (append newline), lee la respuesta en buffer, parsea con `QJsonDocument`, cierra canal.

## Firma Ed25519 y headers

Cada request incluye un header `X-Key-Fingerprint` con el fingerprint de la clave pública. Antes de enviar, el transport firma el cuerpo con la clave privada usando `crypto_sign_detached` y añade `X-Signature`. El daemon valida la firma antes de ejecutar handlers.

## Reintentos y timeouts

- `probeDaemon` usa timeout de 2s; si falla marca `isDaemonAvailable=false` y planifica reintentos cada 30s.
- `call` usa un timeout de 5s y captura errores `SSH_ERROR_CHANNEL_NOT_OPEN` para forzar fallback.

## Logging y tracing

El transport debe trazar: método, host, fingerprint, duración y error (HTTP-like). Los logs se integran con `appendConnectionLog`.

## Pruebas

- Crear un stub local de libssh que reemplace la sesión con un proceso simulado y responde a `/health` y `/exec` (para test unitario). La prueba valida que el client reintente y caiga a SSH cuando se simula error 601.
