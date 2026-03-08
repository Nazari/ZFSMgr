# Configuration and INI files

ZFSMgr uses a per-user configuration directory per OS:

- Linux: `$HOME/.config/ZFSMgr`
- macOS: `$HOME/.config/ZFSMgr`
- Windows: `%USERPROFILE%/.config/ZFSMgr`

## File layout

- `config.ini`: global app configuration.
- `conn*.ini`: one file per connection (for example `conn_fc16.ini`, `conn_surface_psrp.ini`).

Real example:

```text
~/.config/ZFSMgr/
  config.ini
  conn_fc16.ini
  conn_surface_psrp.ini
  conn_mbp_local.ini
```

## What is stored where

- `config.ini`:
  - UI language
  - global log options
  - default values (for example `[ZPoolCreationDefaults]`)
- `conn*.ini`:
  - full definition of one specific connection (host, port, user, key, etc.)

## Startup loading

On startup, ZFSMgr:

1. Reads `config.ini`.
2. Scans the config directory for all `conn*.ini` files.
3. Loads every connection file found.

If legacy format is detected (connections embedded in `config.ini`), ZFSMgr auto-migrates to `conn*.ini` files.
