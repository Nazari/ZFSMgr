# Diff action

> **How to invoke it.** Select the source in the **left** pane and open the context menu **on the node in the right pane**: the `With source …` submenu offers this action. If it is greyed out, the reason is in its tooltip. See `Menus and actions`.

`Diff` compares:

- in `Source`: a snapshot
- in `Target`: the current parent dataset of that snapshot, or another snapshot of the same dataset

Restrictions:

- **Requires the agent** on the connection: it used to compare with `zfs diff` over SSH and
  that path has been withdrawn. Without an agent the reason is given instead of trying.
- Source and Target must be in the same connection
- Source and Target must be in the same pool
- both must refer to the same base dataset

ZFSMgr runs:

```sh
zfs diff -H <source> <target>
```

Valid examples:

- `pool/ds@s1` against `pool/ds`
- `pool/ds@s1` against `pool/ds@s2`

Result:

- a window opens with four root nodes:
  - `Added`
  - `Deleted`
  - `Modified`
  - `Renamed`
- underneath, the files and directories reported by `zfs diff` are shown hierarchically
- renames show the new path and keep the previous path in the tooltip

Progress and timeout:

- as `zfs diff` emits lines they are recorded under `Progress`
- the timeout is based on inactivity, not on total duration
- if there is no output, ZFSMgr reports the remaining time before the timeout under `Progress` every 10 seconds

The window is informational only and is closed with `OK`.
