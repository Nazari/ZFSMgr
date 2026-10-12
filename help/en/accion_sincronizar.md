# Action: Sync

> **How to invoke it.** Select the source in the **left** pane and open the context menu **on the node in the right pane**: the `With source …` submenu offers this action. If it is greyed out, the reason is in its tooltip. See `Menus and actions`.

Goal: synchronize source dataset content into target dataset.

Conditions:

- Source: dataset selected.
- Target: dataset selected.
- Source and target must be different and both mounted. `canmount=off` is also accepted when the equivalent child datasets are mounted on both ends.
- Source and target must run OpenZFS `2.3.3` or newer.

Behavior:

- **Between two datasets on the same machine** it uses the agent's own copy (`--mutate-copy-local`) on every platform: no `rsync` needed. It really synchronizes: it skips what is already identical, keeps owner, mode, timestamps, links, special files, holes, ACLs and extended attributes, honours `--delete`, and can simulate. Without an agent there is **no sync**: the reason is given before the options dialog opens.
- **Between two Unix machines** the interface does not sync: it says so and points to the `rsync` command of `zfsmgr-cli`, which does it with the tree between daemons. It comes back to the interface in 2.0.
- On Windows **between different machines** it still uses `tar` over SSH (with `zstd` or `gzip` when both ends have them) and **without `--delete`**: there it does not synchronize, it copies.
- **On Windows, Sync does not delete at the destination.** That `tar` pipeline adds and
  overwrites, but does not remove what is left over: the delete checkbox does not apply
  there. Verified from a Unix machine to a Windows one. Within one machine it does delete.
- On Linux, macOS and FreeBSD an unmounted dataset can still be synchronized through an alternate temporary mount: the daemon relocates its mountpoint, transfers, and restores it when finished.
- Before queuing, the *Sync options* dialog opens, where you can enable or disable `--delete` (not available in the cross-machine tar mode) and run a `Check` (dry run) whose output is shown in the dialog itself.
- **It runs when you press it.** It used to be added to a pending-changes list and wait for
  you to apply it; that list is gone.
- The copy's summary is written to the log when the operation finishes.
- The `Check` (dry run) can be cancelled from the dialog. The real run, once applied, cannot be cancelled.
- If either side is below `2.3.3`, the action is blocked.

Note about interruptions:

- If a sync using an alternate temporary mount is interrupted abruptly, the dataset may be left mounted on a temporary directory. Use `Repair temporary mountpoints` in the connection context menu to restore it.
