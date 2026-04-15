# Inline properties and columns

ZFSMgr shows dataset and pool properties directly inside the unified tree.

## Where they appear

- On datasets, under `Dataset properties`.
- On snapshots, under `Snapshot properties`.
- On non-snapshot datasets, `Permissions` may also appear.
- On the merged pool/root-dataset node, `Pool Information` may appear.
  - It may include `Devices` (pool vdev/disk hierarchy).
- On filesystem datasets, snapshots hang under the `@` node.
- On pools with active GSA datasets, `Scheduled datasets` may appear.
- On connections, under `Connection properties`.
- On connections, `Info` is grouped into:
  - `General`
  - `GSA`
  - `Commands`

## Visual layout

- The number of visible property columns is configured from the tree header context menu.
- Current `Property columns` values are:
  - `4, 6, 8, 10, 12, 14, 16`
- Column widths are persisted and restored.
- Vertical scrolling in the tree is smooth.

## Visible properties management

Right click on:

- `Dataset properties`
- `Snapshot properties`
- `Pool Information`
- `Connection properties`

to open `Manage visible properties`.

That dialog lets you:

- choose visible properties
- reorder them with drag and drop
- create groups
- rename groups
- delete groups

Groups are independent for:

- pool
- dataset
- snapshot
- connection

## Inline editing

- Editable properties are changed directly in the tree.
- Inheritable properties show `Inh.` when applicable.
- ZFS permissions are also edited inline, but in draft mode.
- `org.fc16.gsa:*` properties do not expose a visual inheritance control.
- Clicking a line in `Pending changes` makes ZFSMgr try to focus the affected object and section.
