---
id: daemon-transport-protocol
title: Protocolo del transport del daemon
---

## Overview

El transport entre ZFSMgr y el daemon remoto usa un canal `libssh` autenticado con clave Ed25519. El cliente establece la sesión, realiza un handshake sencillo y envía mensajes JSON con los campos `id`, `method` y `params`. El contrato compartido vive en `src/daemon_rpc_protocol.h` y se compone de:

- `RpcRequest`
- `RpcResponse` / `RpcResult`
- `PoolInfoJson`
- `PoolDetailsJson`
- `DSInfoJson`
- `PermissionsJson`
- `HoldsJson`
- `GsaPlanJson`

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

Si hay un fallo se envía `error` con `code` y `message` y `result` queda vacío.
`code` sigue convenciones HTTP: 2xx éxito, 4xx cliente, 5xx servidor. Las rutas que exigen fallback (`rsync`, `send`) devuelven `code: 601` y un `result` con `fallback: "ssh"`.

## Handshake y autenticación

1. El cliente firma el nonce (timestamp + random) con su clave privada y envía `X-Key-Fingerprint` en el encabezado SSH.
2. El daemon valida el fingerprint contra `/etc/zfsmgr/fingerprint` y responde `220 Ready` o `401 Unauthorized`.
3. El transporte permite cerrar la sesión si el handshake falla y marca el host como “sin daemon”.

## Retries y timeouts

El transport mantiene un timeout corto (5s). Si no hay respuesta se marca `DaemonTransport::isDaemonAvailable` como false y se programa un reintento después de 30s. La detección se realiza antes de cada acción: si el daemon responde con `/health` ok, se usa; si no, se cae a SSH y se loga el evento.

## Logging

Cada petición incluye `audit_id` (unacron). El daemon genera trazas `RPC [method] -> code` y el cliente agrega `DaemonTransport` logs con `host`, `method`, `Fingerprint`, `resultCode`.

## Estructura JSON de respuestas clave

Para mantener compatibilidad con el modelo interno de ZFSMgr, los `payload` devueltos por las rutas `/pools`, `/pools/details`, `/datasets`, `/permissions`, `/holds` y `/gsa` siguen estas estructuras. Las clases C++ correspondientes son `PoolInfoJson`, `PoolDetailsJson`, `DSInfoJson`, `PermissionsJson`, `HoldsJson` y `GsaPlanJson`.

- `PoolInfoJson`:
  ```json
  {
    "pool": "tank1",
    "state": "ONLINE",
    "version": 50,
    "guid": "abcd-1234",
    "importable": false,
    "comment": "ZPool",
    "children": ["tank1/aux", "tank1/data"]
  }
  ```
- `PoolDetailsJson`:
  ```json
  {
    "pool": "tank1",
    "propsRows": [
      ["size", "1.81T", "local"],
      ["health", "ONLINE", "local"]
    ],
    "statusText": "pool: tank1\n state: ONLINE\n ..."
  }
  ```
- `DSInfoJson`:
  ```json
  {
    "dataset": "tank1/aux",
    "type": "filesystem",
    "mountpoint": "/tank1/aux",
    "canmount": "on",
    "mounted": "yes",
    "encryption": "aes-256-gcm",
    "mountpointVisible": true,
    "snapshots": ["tank1/aux@hourly-1"],
    "properties": {"compression":"lz4","quota":"10G"}
  }
  ```
- `PermissionsJson`:
  ```json
  {
    "dataset": "tank1/aux",
    "localGrants": [
      {"scope":"local","targetType":"user","targetName":"alice","permissions":["create","mount"]}
    ],
    "descendantGrants": [],
    "localDescendantGrants": [],
    "createPermissions": ["snapshot"],
    "permissionSets": [
      {"name":"@backup","permissions":["send","hold"]}
    ],
    "systemUsers": ["alice","bob"],
    "systemGroups": ["staff","wheel"]
  }
  ```
- `HoldsJson`:
  ```json
  {
    "dataset": "tank1/aux@snap-1",
    "holds": [
      {"tag":"backup","timestamp":"1710000000"}
    ]
  }
  ```
- `GsaPlanJson`:
  ```json
  {
    "enabled": true,
    "recursive": true,
    "classes": ["hourly","daily"],
    "destinations": [
      {"connection":"MBP","pool":"tank1","dataset":"backup"}
    ]
  }
  ```

El handler `/pools/details` devuelve un `payload` con `pool`, `propsRows` y `statusText`.
El handler `/permissions` devuelve un `payload` con los bloques de permisos local/descendiente, los conjuntos de permisos y las listas del sistema. El handler `/holds` devuelve un `payload` con la lista de holds del snapshot o dataset solicitado.
El handler `/datasets/{dataset}/properties` devuelve un `payload` con `object`, `datasetType`, `properties` (mapa) y `gsaStatus` si existe. `/actions/{dataset}` añade `command` y `fallback` si se requiere SSH.

### RpcRequest / RpcResponse

- `RpcRequest`:
  - `id`: identificador único por petición.
  - `method`: ruta RPC, por ejemplo `/health` o `/datasets`.
  - `params`: objeto JSON con los argumentos.
- `RpcResponse`:
  - `id`: eco del identificador.
  - `result`: objeto con `code`, `message`, `payload` y, cuando aplica, `fallback`.
  - `error`: `null` o un objeto con `code`, `message` y `details`.

## Next steps for implementation

1. Crear la clase `DaemonRpcMessage` para serializar/deserializar los JSON anteriores y validar ids.
2. Implementar `DaemonTransport::call` usando libssh para abrir canal, enviar `rpc-method`, escuchar `result` y convertirlo en `DaemonRpcResult`.
3. Preparar tests contra un stub que responde con los mismos JSON y evalúa fallback.
