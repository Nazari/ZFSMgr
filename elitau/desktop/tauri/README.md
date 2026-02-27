# ZFSMgr Tauri Shell

Shell nativo (Tauri) para el backend Elixir de ZFSMgr.

## Requisitos

- Rust (`rustup`, `cargo`, `rustc`)
- Node.js 20+
- Backend Elixir funcionando en `http://127.0.0.1:4001`

## Arranque backend

Desde `elitau/`:

```bash
export ZFSMGR_MASTER_PASSWORD='tu_password_maestra'
mix deps.get
mix ecto.create
mix ecto.migrate
iex -S mix
```

## Arranque Tauri (desarrollo)

Desde `elitau/desktop/tauri/`:

```bash
npm install
npm run dev
```

La UI permite cambiar la URL base de API arriba a la derecha.

## Build nativo

```bash
npm run build
```

Generará los instaladores/binarios nativos por sistema operativo con Tauri.
