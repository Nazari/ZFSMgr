# Diff action

> **How to invoke it.** Right-click the source (`Mark as source`), then open the context menu **on the target node**: the `With source …` submenu offers this action. There is no button any more. If it is greyed out, the reason is in its tooltip. See `Context menus`.

`Diff` compares:

- in `Source`: a snapshot
- in `Target`: the current parent dataset of that snapshot, or another snapshot of the same dataset

Restrictions:

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
