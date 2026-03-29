# Inline properties and columns

ZFSMgr shows dataset and pool properties directly inside the unified tree.

## Where they appear

- On datasets and snapshots, under `Dataset properties`.
- On non-snapshot datasets, `Permissions` may also appear.
- On the merged pool/root-dataset node, `Pool Information` may appear.
- On filesystem datasets, `Schedule snapshots` may appear.
- On pools with active GSA datasets, `Datasets programados` may appear.

## Visual layout

- The number of visible property columns is configured from the tree header context menu.
- Current `Property columns` values are:
  - `4, 6, 8, 10, 12, 14, 16`
- Column widths are persisted and restored.
- Vertical scrolling in the tree is smooth.

## Visible properties management

Right click on:

- `Dataset properties`
- `Pool Information`

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

## Inline node visibility

From the tree context menu you can toggle:

- `Show inline properties`
- `Show inline permissions`
- `Show Pool Information`
- `Show Scheduled Datasets`

Effects:

- disabling `Show inline properties` hides `Dataset properties`
- disabling `Show inline permissions` hides `Permissions`
- disabling `Show Pool Information` hides `Pool Information`
- disabling `Show Scheduled Datasets` hides `Datasets programados`

## Inline editing

- Editable properties are changed directly in the tree.
- Inheritable properties show `Inh.` when applicable.
- ZFS permissions are also edited inline, but in draft mode.
- `org.fc16.gsa:*` properties do not expose a visual inheritance control.
- Clicking a line in `Pending changes` makes ZFSMgr try to focus the affected object and section.
