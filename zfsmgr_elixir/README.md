# ZFSMgr Elixir (Migration Base)

Primera base de migracion de ZFSMgr Python a Elixir.

Incluye:

- Persistencia con `Ecto + SQLite`.
- Esquemas para conexiones y log de acciones.
- Contexto `ZfsmgrElixir.Core`.
- Supervisor base de sesiones por conexion (`DynamicSupervisor`).
- Cifrado simetrico minimo para secretos en `ZfsmgrElixir.Crypto`.

## Estado actual

Esta fase no implementa aun UI LiveView ni ejecucion real de comandos ZFS/SSH/PSRP.
Es la base de datos + arquitectura de procesos para avanzar por fases.

## Requisitos

- Elixir 1.18+
- Erlang/OTP 27+

## Comandos

```bash
cd zfsmgr_elixir
mix deps.get
mix ecto.create
mix ecto.migrate
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
