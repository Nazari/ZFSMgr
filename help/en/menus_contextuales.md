# Menus and actions

Managing connections lives in the **menu bar**. What acts ON something selected lives in the
**context menu** of the tree and of the detail tabs.

## The «Connections» menu

![Connections menu](qrc:/help/img/auto/connections-menu.png)

It acts on the connection of whichever pane you touched last. Since that choice is not
visible, the name goes **inside** the label: `Edit "unibody"`, not `Edit`.

- `New Connection`
- `Edit` · `Delete`
- `Connect` · `Disconnect` · `Refresh` · `Refresh all`
- `New Pool`
- `Install helper commands`
- `Reinstall/Update daemon`
- `Repair temporary mountpoints`
- `Export trust-store to this connection`
- `Authorize SSH key on…` (submenu with the other connected SSH connections)
- `Hand over the other machines' credentials…`
- `Change local sudo credentials…` (Local connection only)

Enablement rules:

- `Connect`: connection marked as disconnected and no action in progress.
- `Disconnect` and `Refresh`: connection connected and no action in progress.
- `Edit` and `Delete`: not available on the Local connection nor on connections redirected to
  Local.
- `New Pool`: connection connected.
- `Install helper commands`: only if the refresh detected a package manager and a supported
  install plan for the missing commands. Does not apply to Windows connections, which work
  through the native agent only.
- `Reinstall/Update daemon`: any connection, Windows and Local included, and **also when
  disconnected** — which is exactly when it is usually needed.
- `Repair temporary mountpoints`: any non-Windows connection, Local included (a local dataset
  can also be left on a temporary mountpoint).
- `Export trust-store to this connection`: any remote connection; does not apply to Local,
  which already uses the local trust-store.
- `Authorize SSH key on…`: SSH non-Windows connections only, and it is only populated with
  other connected SSH connections.
- `Change local sudo credentials…`: Local only, the one connection that cannot be edited. It
  is offered even when disconnected: a wrong password is what may have left it that way.

`Repair temporary mountpoints` first does a read-only pass, shows the datasets left with a
relocated mountpoint by an interrupted sync, and asks for confirmation before restoring them
(unmounting them first). Those that fail keep their mark and can be retried.

## Automatic daemon update

After a refresh, ZFSMgr reinstalls the daemon automatically and without dialogs **only** when
the reason for attention is a version or API mismatch.

A daemon-rpc TLS backoff marks the connection for attention but does **not** trigger the
automatic reinstall: reinstalling would regenerate the TLS material and perpetuate the
failure → reinstall → failure loop. In that case use `Reinstall/Update daemon` or
`Export trust-store to this connection` manually.

## On the pool root node

![Imported pool context menu](qrc:/help/img/auto/pool-context-menu-imported.png)

The pool node and its root dataset are the same item, so the menu carries both: first a
`Pool` submenu and then the normal dataset actions.

Inside `Pool`:

- `Refresh status`
- `Import` · `Import renaming` · `Export`
- `History`
- `Management`: `Sync`, `Scrub`, `Upgrade`, `Reguid`, `Trim`, `Initialize`, `Clear`,
  `Destroy` — immediate actions, with a parameter dialog where it applies.

## On datasets

- `Dataset`:
  - `Create` · `Rename` · `Delete`
  - `Mount`: only if the dataset has `canmount` other than `off`, a valid `mountpoint` and is
    **not** already mounted.
  - `Unmount`: only if it **is** mounted. `canmount` is not required: a dataset can be
    mounted and then have `canmount=off`, and that is exactly when this is needed.
  - `Encryption key`: `Load key`, `Unload key`, `Change key`
  - `Schedule snapshots`
  - `Permissions`: `New set`, `New delegation`
- `Actions` (operations on the DATA, not on the dataset's state):
  `Break down`, `Assemble`, `From Dir`, `To Dir`
- `With source …` — the six two-ended actions; see below.

## On snapshots

In the detail's `Snapshots` tab, with the same menu they had inside the tree:

- `Delete snapshot` · `Rollback` · `New Hold`
- `With source …`

And in the `Holds` tab, on a row: `Release`.

## The six source-and-destination actions

`Send`, `Move`, `Clone`, `Sync`, `Level` and `Diff` need **two** ends, and each pane supplies
one: the left is the source and the right the destination.

1. Select the starting dataset or snapshot in the **left** pane.
2. Right-click the node in the **right** pane: that is the destination, and the
   `With source <name>` submenu offers all six, naming the source in each:

```
With source datos@monday ▸
   Send here from datos@monday
   Move here from datos@monday
   Clone here from datos@monday
   Sync here from datos@monday
   Level with datos@monday
   Compare with datos@monday
```

Selecting a snapshot in the `Snapshots` tab counts the same as selecting it in the tree.

What does not apply is shown **greyed out, with the reason in its tooltip**: the source is not
a snapshot, the pools do not match, `Diff` compares two points of the same dataset, or the
OpenZFS versions are not compatible for transfer.

**`Move` copies nothing.** It is a `zfs rename`: the dataset moves within the tree and the
data stays where it is, so it is instant and there is no original left to delete. That is why
it only works **within the same pool and the same machine**, and with datasets on both ends —
never snapshots. To take something to another pool or another machine, that is `Send`. What
does change is that dataset's mount path and that of everything below it.

## Rules

- Destructive actions ask for confirmation.
- **Properties**, **permissions** and **renames** are edited as drafts and applied with
  `Apply changes`. Actions are not: they run when pressed.
- The `Unapplied changes` box lists exactly what those buttons will do.
- On suspended pools, most context menu actions appear disabled.
