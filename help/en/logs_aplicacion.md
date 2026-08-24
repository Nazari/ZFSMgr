# Logs

**Each connection's log sits under its pane**, not at the bottom of the window. They all used
to be in tabs down there, next to a `Combined log`: looking at one machine's log meant hunting
for its tab, and with two panes on two machines, going back and forth between two tabs.

Each pane has two:

- `Log · <machine>`: technical output of that machine's commands.
- `Daemon · <machine>`: its daemon log (`/var/lib/zfsmgr/daemon.log`, or
  `C:\ProgramData\ZFSMgr\agent\daemon.log` on Windows) and the `Heartbeat` button.

If the same connection is chosen in both panes, both show the same log: the text is not
duplicated and the lines are not split between two places.

The `Combined log` was removed. The **application** log is still written to disk and copied
from `Settings ▸ Logs ▸ Copy`.

At the very bottom there are two boxes: `Unapplied changes` and `Transfers`.

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

## Transfers box

- Shows one row per background transfer job (daemon-to-daemon Send or Level).
- Each row shows: state, source/target datasets, bytes transferred, speed, elapsed time.
- Possible states: `running`, `done`, `failed`, `cancelled`.
- `Refresh` forces an immediate status query to the daemons.
- `Cancel selected` sends `SIGTERM` to the `zfs send` process of the selected job.
- Running jobs are recovered automatically on reconnect.

## Unapplied changes box

- Lists, line by line, what `Apply changes` and `Discard changes` will do: every edited
  property and every touched permission delegation, with its connection and its object.
- It serves **only** property and permission drafts: actions are not queued, they run when
  pressed.
- `Discard changes` says what will be lost before throwing it away.

## Initial load on startup

When ZFSMgr starts:

- Persisted logs are read (`application.log` and rotated files `.1` ... `.5`).
- Only the last `N` lines are loaded into the view.
- `N` is the configured maximum lines limit from the `Settings ▸ Logs` menu.
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
