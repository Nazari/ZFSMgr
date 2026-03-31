---
id: daemon-transport-protocol
title: Protocolo del transport del daemon
---

## Overview

El transport entre ZFSMgr y el daemon remoto usa un canal `libssh` autenticado con clave Ed25519. El cliente establece la sesión, realiza un handshake sencillo y envía mensajes JSON con un header `rpc-method` y un body `params`.

Cada petición tiene el formato:

```json
{
  "id": "uuid",
  "method": "/datasets/mount",
  "params": {
     "conn": "MBP",
     "dataset": "tank1/void",
     "action": "mount",
     "recursive": false
  }
}
```

El daemon responde con:

```json
{
  "id": "uuid",
  "result": {
      "code": 200,
      "payload": {
          "status": "ok",
          "message": "mounted"
      }
  },
  "error": null
}
```

Si hay un fallo se envía `error` con `code` y `message` y `result` queda vacío
(`code` sigue convenciones HTTP: 2xx éxito, 4xx cliente, 5xx servidor). Las rutas que exigen fallback (`rsync`, `send`) devuelven `code: 601` y un payload `{ "fallback": "ssh" }`.

## Handshake y autenticación

1. El cliente firma el nonce (timestamp + random) con su clave privada y envía `X-Key-Fingerprint` en el encabezado SSH.
2. El daemon valida el fingerprint contra `/etc/zfsmgr/fingerprint` y responde `220 Ready` o `401 Unauthorized`.
3. El transporte permite cerrar la sesión si el handshake falla y marca el host como “sin daemon”.

## Retries y timeouts

El transport mantiene un timeout corto (5s). Si no hay respuesta se marca `DaemonTransport::isDaemonAvailable` como false y se programa un reintento después de 30s. La detección se realiza antes de cada acción: si el daemon responde con `/health` ok, se usa; si no, se cae a SSH y se loga el evento.

## Logging

Cada petición incluye `audit_id` (unacron). El daemon genera trazas `RPC [method] -> code` y el cliente agrega `DaemonTransport` logs con `host`, `method`, `Fingerprint`, `resultCode`.

## Next steps for implementation

1. Crear la clase `DaemonRpcMessage` para serializar/deserializar los JSON anteriores y validar ids.
2. Implementar `DaemonTransport::call` usando libssh para abrir canal, enviar `rpc-method`, escuchar `result` y convertirlo en `DaemonRpcResult`.
3. Preparar tests contra un stub que responde con los mismos JSON y evalúa fallback.
