# Application logs

The bottom area uses tabs:

- `Pending changes`: what will be applied when you press `Apply changes`.
- `Settings`: log options and action confirmation.
- `Combined log`: main application log.
- `Transfers`: list of background daemon transfer jobs (Copy/Level daemon-to-daemon).

Inside the `Combined log`, **each connection** also has its own sub-tabs:

- `Terminal`: technical output of that machine's commands.
- `Daemon`: its daemon log (`/var/lib/zfsmgr/daemon.log`, or
  `C:\ProgramData\ZFSMgr\agent\daemon.log` on Windows) and the `Heartbeat` button.

## Daemon tab

- Shows the remote daemon log read incrementally from `/var/lib/zfsmgr/daemon.log`.
- The `Heartbeat` button pings the daemon to confirm it is responsive.
- The log updates when a ZED event is detected or when `Heartbeat` is pressed.
- The log is not cleared on connection refresh; it resets only if the daemon is reinstalled.
- daemon-rpc failures appear in the logs as `daemon-rpc:fallback` or `daemon-rpc:skip`,
  followed by a stable tag naming the kind of failure (`tls-handshake`,
  `conexion-rechazada`, `tunel-ocupado`…). That tag is deliberately not translated: it is
  what you grep for in a log that may come from a machine set to another language.
- **On a TLS failure, ZFSMgr does NOT reinstall the daemon or rebuild the material on its
  own**: it flags the connection for attention and waits. Re-provisioning would regenerate
  the TLS material and keep the failure → reinstall → failure loop going. Automatic
  reinstall only happens when the reason is a version or API mismatch.

## Transfers tab

- Shows one row per background transfer job (daemon-to-daemon Copy or Level).
- Each row shows: state, source/target datasets, bytes transferred, speed, elapsed time.
- Possible states: `running`, `done`, `failed`, `cancelled`.
- `Refrescar` forces an immediate status query to the daemons.
- `Cancelar seleccionado` sends `SIGTERM` to the `zfs send` process of the selected job.
- Running jobs are recovered automatically on reconnect.

`Combined log`:

- Includes internal application events.
- Includes relevant execution output in compact format.

## Initial load on startup

When ZFSMgr starts:

- Persisted logs are read (`application.log` and rotated files `.1` ... `.5`).
- Only the last `N` lines are loaded into the view.
- `N` is the configured maximum lines limit from `Settings`.
- If logs do not exist or are empty, no error is shown.

## Compact on-screen rendering

Each new line is compared against the previous visible line.  
The view shows only changes in:

- Date.
- Time.
- Connection.
- Log level.

If none of those fields changes, `...` is shown as compact header.

Visual format:

- `<changes> | <message>`

## Persistence

- Full-format lines are still stored on disk for traceability.
- Compact rendering is only applied in the on-screen view.
