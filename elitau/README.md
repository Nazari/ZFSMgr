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
  - `POST /api/connections/:id/actions/import_pool`
  - `POST /api/connections/:id/actions/export_pool`
  - `POST /api/connections/:id/actions/create_dataset`
  - `POST /api/connections/:id/actions/delete_dataset`
  - `POST /api/connections/:id/actions/rename_dataset`
  - `POST /api/connections/:id/actions/set_property`
  - `POST /api/connections/:id/actions/inherit_property`
  - `GET /api/connections/:id/pools`
  - `GET /api/connections/:id/datasets?pool=<pool>`
  - `GET /api/dataset_properties/editable`
  - `GET /api/logs?limit=500`
- Worker por conexion (`ConnectionSession`) con `refresh` real.
- Servicio reusable `ZfsmgrElixir.Zfs` para acciones ZFS (usable desde otras apps Elixir).

Aun no implementa:

- UI LiveView.
- PSRP real (por ahora devuelve `:psrp_not_implemented`).

## Requisitos

- Elixir 1.18+
- Erlang/OTP 27+

## Comandos

```bash
cd elitau
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
cd elitau/desktop/tauri
npm install
npm run tauri dev
```
