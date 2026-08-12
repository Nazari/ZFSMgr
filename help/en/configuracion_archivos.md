# Configuration and files

ZFSMgr uses one configuration directory per user and operating system:

- Linux: `$HOME/.config/ZFSMgr`
- macOS: `$HOME/.config/ZFSMgr`
- Windows: `%USERPROFILE%/.config/ZFSMgr`

## File layout

- `config.json`: global application settings and connection definitions. Sensitive fields (user and password) are stored encrypted with the master password.
- `trust-store.json`: the daemon's TLS material (server certificate, plus client certificate and key), also encrypted with the master password. This is the file copied by `Export trust-store to this connection`.
- `application.log`: the application's persistent log.

Actual example:

```text
~/.config/ZFSMgr/
  config.json
  trust-store.json
  application.log
```

## What is stored in `config.json`

The root object has three blocks:

- `connections`: an array with the full definition of each connection (id, name, machine_uid, type, operating system, host, port, SSH address family, user, password, key path and sudo usage). User and password are encrypted.
- `app`: application options and interface state.
- `ZPoolCreationDefaults`: default values for pool creation.

Inside `app` the following are stored, among others:

- interface language
- log options (maximum size, level, number of lines)
- action confirmation
- number of property columns (`conn_prop_columns`)
- connection shown in the upper panel
- connections marked as disconnected
- window geometry and splitter state
- visibility of the inline sections
- order and groups of inline properties

## What is NOT stored

- The `Source` mark is per session: it is lost when the application closes. There is no
  target to store: it is the node you request the action on.
- The **pending changes list** IS stored (`pending_actions` key), with each action's
  command and WITHOUT passwords: they are replaced by a marker and restored from the
  connection on load.
- Tree column widths are kept when switching connection or panel within the same session, but not across restarts.

## Loading at startup

On startup, ZFSMgr:

1. Migrates the legacy configuration if present (see below).
2. Reads `config.json`.
3. Loads the connections from the `connections` array.
4. Moves to `trust-store.json` any TLS material still stored in `config.json`.

## Migration from the old format

If old `config.ini`, `connections.ini` or `conn*.ini` files exist, ZFSMgr merges them automatically into `config.json` — the `connection:<id>` groups become entries of the `connections` array — and then deletes them. The INI format is no longer used.
