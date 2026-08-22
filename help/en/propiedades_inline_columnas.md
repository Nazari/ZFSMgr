# Inline properties and columns

ZFSMgr shows dataset and pool properties directly inside the unified tree.

## Where they appear

- On datasets, under `Dataset properties`.
- On snapshots, under `Snapshot properties`.
- On non-snapshot datasets, `Permissions` can also appear.
- On the dual pool/root-dataset node, `Pool Information` can appear.
  - Inside it, `Devices` may appear (the pool's vdev/disk tree).
- On filesystem datasets, snapshots hang from the `@` node.
- On pools with active GSA datasets, `Scheduled datasets` can appear.
- On connections, under `Properties`.
- On connections, `Info` groups:
  - `General`
  - `Daemon`
  - `Commands`

## Display

- The number of visible columns is set from the tree header's context menu.
- The current range of `Property columns` is:
  - `4, 6, 8, 10, 12, 14, 16`
- Column widths are kept when switching connection or panel within the same session; they are not kept across application restarts.
- The tree scrolls smoothly.

## Managing visible properties

Right-clicking on:

- `Dataset properties`
- `Snapshot properties`
- `Pool Information`

opens `Manage property display`.

The connection's `Properties` node has no context menu: its fields are edited inline directly.

That dialog lets you:

- choose the visible properties
- reorder them by drag and drop
- create groups
- rename them
- delete groups

Groups are independent per:

- pool
- dataset
- snapshot

## Inline editing

- Editable properties are modified directly in the tree.
- Inheritable properties show `Inh.` where it applies.
- ZFS permissions are also edited inline, but in draft mode.
- User properties (those with a `:` in the name, such as `org.fc16.gsa:*`) are editable and also show the inheritance control.
- Read-only properties, properties that do not apply to the platform, and `canmount` do not show the inheritance control.
- The pending-changes list is gone, and so is jumping to the object from it. Property
  drafts are shown highlighted in the grid itself.
