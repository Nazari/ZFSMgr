# Properties, permissions and content

All of this lives in each pane's **detail**, below its tree. It used to live inside the tree,
with the values laid sideways across unlabelled `C1`…`C10` columns; with more than four
properties you had to read them in a zigzag, and the tree could no longer be scanned at a
glance.

![Detail tabs](qrc:/help/img/auto/detail-tabs.png)

## The path

At the top of the detail, what you are looking at: `Local / tank1 / user`. The first two
segments are links and bring up their card without losing the tree selection:

- **The connection** — the profile fields (name, type, host, port, user, key, sudo) and the
  diagnosis: status, colour reason, operating system, OpenZFS version, daemon state, package
  manager, installable commands.
- **The pool** — its properties and its `zpool status`.

These two cards show their data **two pairs per row** — "Property · Value · Property · Value"
— with a heavier rule between the two groups.

## Dataset properties

The first tab, and the only **editable** one.

- Closed-value ones show as a dropdown.
- Inheritable ones carry a checkbox in the `Inherited` column.
- User properties — the ones with `:` in the name, such as `org.fc16.gsa:*` — are editable and
  also carry the inheritance control.
- Read-only ones, ones that do not apply to the platform, and `canmount` carry no inheritance
  control.
- The first rows — name, mountpoint, `canmount`, size — are pinned at the top.

What you edit is **not applied immediately**: it piles up as a draft and is listed in the
`Unapplied changes` box below. It is applied with `Apply changes` or thrown away with
`Discard changes`.

## Content

The files under the dataset's mountpoint, with columns of its own: name, permissions, owner,
group, size and modification date.

Each directory is fetched **when opened**, not before: a dataset can hold thousands of files.
If the dataset is not mounted, the tab says so instead of sitting empty.

On a snapshot it browses its `.zfs/snapshot`, which is where ZFS exposes it.

## Snapshots

Grouped by class, just as they were in the tree: `Hourly`, `Daily`, `Weekly`, `Monthly`,
`Yearly`. Hand-made ones stand alone, with no group.

Selecting one here selects it **as that pane's source or destination**, the same as selecting
it in the tree: `Send`, `Clone`, `Sync`, `Level` and `Diff` depend on that. And the detail
switches to that snapshot: its properties, its content and its holds.

## Permissions

Each delegation is a row — who and with what scope — and below it its permissions with
checkboxes.

They are read **when the tab is opened**, not when the dataset is selected: reading them with
the selection would mean a remote call for every cursor move. Until they have been read, the
tab says so.

Ticking or unticking a permission rewrites the whole delegation, because `zfs allow` takes the
full list for whoever is delegated to. It is a draft, like properties: it shows up in
`Unapplied changes` and waits for `Apply changes`.

Creating a new delegation lives in the dataset's context menu, under `Dataset ▸ Permissions`.

## Holds

Snapshots only: each hold with its name and its date. Releasing one comes from its row's
context menu. Creating a hold lives in the snapshot's context menu.

## Which tab is shown

The one that does not apply is **not shown**: a snapshot delegates no permissions and a
dataset has no holds. On a dataset you get `Properties`, `Content`, `Snapshots` and
`Permissions`; on a snapshot, `Properties`, `Content`, `Snapshots` and `Holds`.
