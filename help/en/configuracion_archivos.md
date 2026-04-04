# Configuration and INI files

ZFSMgr uses a per-user configuration directory per OS:

- Linux: `$HOME/.config/ZFSMgr`
- macOS: `$HOME/.config/ZFSMgr`
- Windows: `%USERPROFILE%/.config/ZFSMgr`

## File layout

- `config.ini`: global app configuration.

Real example:

```text
~/.config/ZFSMgr/
  config.ini
```

## What is stored in `config.ini`

- UI language
- global log options
- property columns count (`conn_prop_columns`)
- selected source/target connection+dataset
- splitter state and window geometry
- unified-tree column widths
- inline properties order and groups
- default values (for example `[ZPoolCreationDefaults]`)
- full connection definitions in `connection:<id>` groups

## Startup loading

On startup, ZFSMgr:

1. Reads `config.ini`.
2. Loads connections from `connection:<id>` groups.

If old `conn*.ini` files exist, ZFSMgr auto-migrates them into `config.ini` and then deletes them.
