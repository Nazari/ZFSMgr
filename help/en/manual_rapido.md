# Quick manual

ZFSMgr works with **two panes**: the left one is the SOURCE and the right one the
DESTINATION. Position is not a layout preference: it is what decides the role each selection
plays in transfer actions.

## Overview

![Main window](qrc:/help/img/auto/main-window.png)

Top to bottom:

- **Menu bar**: `Menu`, `Connections`, `Settings`, `Help`.
- **Status and Progress**, on one line: what is happening and the last message.
- **The two panes**, each in three rows separated by splitters:
  - **Dropdowns** for connection and pool.
  - **Tree** of pools and datasets.
  - **Detail** of whatever is selected, with its tabs.
  - **Log** of that pane's connection, with `Log` and `Daemon`.
- At the bottom, two boxes: **Unapplied changes** and **Transfers**.

The splitters are **shared by both panes**: moving the boundary between tree and detail moves
it on both sides at once, so the two columns stay readable side by side. There is no splitter
between the left and right panes: the width is split in half.

## Choosing what you see

Each pane has two dropdowns:

- **Connection**: the machine. Changing it changes that pane's tree, detail and log.
- **Pool**: `(all pools)` shows every pool on that connection; picking one roots the tree in
  it.

If you pick a **pool that is not imported** — marked `[Importable]` — the import dialog opens
with its options. A pool that is not imported has no datasets to show, so picking it is
asking for it to be imported.

## The tree

It holds **pools and datasets, and nothing else**. Properties, permissions, content and
snapshots live in the detail below.

- A pool that is not imported is marked `[Importable]`.
- A suspended pool is marked `(Suspended)` and most of its operations are blocked.
- The pool root node is merged with the pool's root dataset: it keeps the pool icon and also
  acts as the root dataset, so `pool/pool` is not duplicated.

## The detail

![Detail tabs](qrc:/help/img/auto/detail-tabs.png)

Below each tree, with the full path of what you are looking at on top:
`Local / tank1 / user`. The first two segments are **links**:

- **The connection** brings up its card — the profile fields and the diagnosis: status,
  colour reason, operating system, OpenZFS version, daemon, package manager.
- **The pool** brings up its own: its properties and its `zpool status`.

They are clicked without losing the tree selection, and selecting anything else goes back to
the object.

With a **dataset** selected there are four tabs:

- `Properties` — editable. Closed-value ones show as a dropdown, and inheritable ones carry
  an `Inherited` checkbox.
- `Content` — the files under the mountpoint. Each directory is fetched when opened.
- `Snapshots` — grouped by class: `Hourly`, `Daily`, `Weekly`, `Monthly`, `Yearly`;
  hand-made ones stand alone.
- `Permissions` — the delegations, each with its permissions and checkboxes.

With a **snapshot** selected, `Permissions` is replaced by `Holds`: a snapshot delegates no
permissions and a dataset has no holds, so the tab that does not apply is not shown.

## Choosing source and destination

- The **source** is what is selected in the left pane; the **destination**, what is selected
  in the right one.
- Selecting a snapshot in the `Snapshots` tab counts the same as selecting it in the tree.
- Actions — `Send`, `Clone`, `Sync`, `Level`, `Diff` — are requested from the destination's
  context menu, the way pasting works.

## Unapplied changes

Actions run when you press them. What **is** edited in batches are **properties** and
**permissions**: they pile up as drafts and are applied with `Apply changes`.

The bottom-left box lists exactly what those buttons will do, one per line. `Discard changes`
throws them away, saying first what will be lost.

Drafts **are lost when you close without applying them**: nothing survives the application
closing.

## Transfers

The bottom-right box shows the **jobs running** in the daemons: what is running and its
progress. `Refresh` asks again, and `Cancel selected` stops the selected one.

## Logs

Under each pane, the log of **its** connection, with two tabs:

- `Log · <machine>` — what has been run on that machine.
- `Daemon · <machine>` — its daemon's log, with a `Heartbeat` button to ping it.

If the same connection is chosen in both panes, both show the same log without duplicating
it.

The application log is not shown in the window: it is written to disk and copied from
`Settings ▸ Logs`.

## Creating pools

![Create pool](qrc:/help/img/crearpool.png)

- `Connections ▸ New Pool` opens the VDEV and pool parameter builder.
- The pool tree structure validates compatible OpenZFS combinations.
- On failure the dialog stays open so you can fix and retry.

## Creating datasets

![Create dataset](qrc:/help/img/creardataset.png)

- `Create dataset` opens from the tree's context menu.
- If the dataset is encrypted with `keylocation=prompt`, ZFSMgr asks for the passphrase.
- On failure the dialog stays open with what you typed.

## Navigation

- Each pane remembers separately what it had expanded: both can sit on the same connection
  without stepping on each other.
- The connection and pool choice survives refreshes: it is looked up by identifier, not by
  position in the list.
