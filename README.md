# ZFSMgr Monorepo

Estructura del repositorio:

- `vpython/`: versión Python (GUI Tkinter original).
- `elitau/`: versión Elixir + Tauri.

## Arranque rápido

### Python

```bash
cd vpython
python3 app.py
```

### Elixir + Tauri

Backend:

```bash
cd elitau
export ZFSMGR_MASTER_PASSWORD='tu_password_maestra'
mix deps.get
mix ecto.create
mix ecto.migrate
iex -S mix
```

Frontend nativo:

```bash
cd elitau/desktop/tauri
npm install
npm run dev
```
