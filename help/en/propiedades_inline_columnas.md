# Inline properties and columns

ZFSMgr can show pool and dataset properties directly inside the detail trees.

## Where they appear

- On a dataset, inline properties appear under the `Properties` node, initially collapsed.
- On a non-snapshot dataset, a separate `Permissions` node can also appear.
- On a pool, inline pool properties appear under `Pool information`.
- On a snapshot, inline properties appear under `Properties`.
- Datasets hang directly from the pool.
- Child datasets hang directly from their parent dataset.

## Permissions

- The `Permissions` node shows ZFS delegations defined with `zfs allow`.
- It is organized as:
  - `Deleg.`
  - `New child DS`
  - `Sets`
- Expanding a delegation or a set shows permissions inline as a two-row grid:
  - top row with names
  - bottom row with checkboxes
- `New child DS` defines which permissions are automatically granted to users creating child datasets below the current dataset.
- Permission checks do not execute commands immediately.
  They update a local draft that is later applied with `Apply changes`.

## Visibility

The tree context menu can toggle:

- `Show inline properties`
- `Show inline permissions`
- `Show pool information`

Effects:

- disabling `Show inline properties` hides the `Properties` node;
- disabling `Show inline permissions` hides the `Permissions` node;
- disabling `Show pool information` hides the `Pool information` node;
- the tree no longer uses intermediate `Content` or `Subdatasets` nodes.

## Columns and layout

- The number of visible property columns is controlled from the tree header menu under `Property columns`.
- The first column keeps the `Source:` prefix in the top tree and `Target:` in the bottom tree.
- Each tree keeps its own column widths.
- Column widths are stored in configuration and restored on startup.
- Vertical scrolling uses per-pixel scrolling for smoother movement.

## Visible property management

Right-click on:

- `Properties` of a dataset, or
- `Pool information` of a pool, or
- `Properties` of a snapshot

to open `Manage property visibility`.

That dialog lets you:

- choose which properties are shown,
- reorder them with drag and drop,
- create or delete display groups,
- persist the selection and order.

Groups are independent for:

- pool
- dataset
- snapshot

In snapshots, the `snapshot` property stays fixed in the first position of the main group.

## Inline editing

- Editable properties can be changed directly in the tree.
- ZFS permissions in `Deleg.`, `New child DS`, and `Sets` are also edited directly in the tree, but only as drafts.
- If a property is inheritable, the label shows `Inh.`.
- In that case, the value cell may include an additional `off/on` control to apply `zfs inherit`.
- Unsupported properties are shown dimmed and cannot be edited on that platform.
