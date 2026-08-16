# Action: Sync

> **How to invoke it.** Right-click the source (`Mark as source`), then open the context menu **on the target node**: the `With source …` submenu offers this action. There is no button any more. If it is greyed out, the reason is in its tooltip. See `Context menus`.

Goal: synchronize source dataset content into target dataset.

Conditions:

- Source: dataset selected.
- Target: dataset selected.
- Source and target must be different and both mounted. `canmount=off` is also accepted when the equivalent child datasets are mounted on both ends.
- Source and target must run OpenZFS `2.3.3` or newer.

Behavior:

- On Unix connections it uses `rsync` through the daemon (`--mutate-rsync-local`): the daemon itself probes rsync's capabilities (`-A` for ACLs, `-X` for extended attributes, `--info=progress2`) and runs the command, without building shell commands. If no daemon is available, the older shell-based mechanism is used.
- On Windows, **between two datasets on the same machine**, it uses the agent's own copy (`--mutate-copy-tree`), which needs no `rsync`. It really synchronizes: it skips what is already identical, honours `--delete`, and can simulate, so `Check` works just as on Unix.
- On Windows **between different machines** it still uses `tar` over SSH (with `zstd` or `gzip` when both ends have them) and **without `--delete`**: there it does not synchronize, it copies.
- On Linux, macOS and FreeBSD an unmounted dataset can still be synchronized through an alternate temporary mount: the daemon relocates its mountpoint, transfers, and restores it when finished.
- Before queuing, the *Sync options* dialog opens, where you can enable or disable `--delete` (not available in the cross-machine tar mode) and run a `Check` (dry run) whose output is shown in the dialog itself.
- The action is queued in `Pending changes` and runs only when changes are applied.
- The rsync output is written to the log when the operation finishes. On the daemon path there are **no real-time progress lines**.
- The `Check` (dry run) can be cancelled from the dialog. The real run, once applied, cannot be cancelled.
- If either side is below `2.3.3`, the action is blocked.

Note about interruptions:

- If a sync using an alternate temporary mount is interrupted abruptly, the dataset may be left mounted on a temporary directory. Use `Repair temporary mountpoints` in the connection context menu to restore it.
