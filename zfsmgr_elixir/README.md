# ZFSMgr Elixir (Migration Base)

Primera base de migracion de ZFSMgr Python a Elixir.

Incluye:

- Persistencia con `Ecto + SQLite`.
- Esquemas para conexiones y log de acciones.
- Contexto `ZfsmgrElixir.Core`.
- Supervisor base de sesiones por conexion (`DynamicSupervisor`).
- Cifrado simetrico minimo para secretos en `ZfsmgrElixir.Crypto`.
- API HTTP base con Plug/Cowboy (`ZfsmgrElixirWeb.Endpoint`).

## Estado actual

Esta fase implementa:

- Persistencia de conexiones y logs.
- API REST base:
  - `GET /api/health`
  - `GET /api/connections`
  - `POST /api/connections`
  - `PUT /api/connections/:id`
  - `DELETE /api/connections/:id`
  - `POST /api/connections/:id/refresh`
  - `GET /api/logs?limit=500`
- Worker por conexion (`ConnectionSession`) con `refresh` base.

Aun no implementa:

- UI LiveView.
- Ejecucion real de comandos ZFS/SSH/PSRP (el refresh de sesion actual es placeholder).

## Requisitos

- Elixir 1.18+
- Erlang/OTP 27+

## Comandos

```bash
cd zfsmgr_elixir
mix deps.get
mix ecto.create
mix ecto.migrate
mix run --no-halt
mix test
```

## Integracion desktop (Tauri)

La estructura de integracion Tauri queda preparada en `desktop/tauri/`.

Para compilar Tauri necesitas Rust instalado (`rustc`, `cargo`) y luego:

```bash
cd zfsmgr_elixir/desktop/tauri
npm install
npm run tauri dev
```
