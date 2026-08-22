# Navigation and states

- The cursor switches to busy during actions and refreshes.
- The unified tree is now the main navigation surface.
- The current tree selection does not replace logical `Source` and `Target`.
- `Source` and `Target` are set explicitly from the dataset context menu.
- The `Source:` line in the top band reflects what is marked. The target is not marked: it is the node whose menu you open to request the action.
- If a connection is disconnected:
  - the connection root stays visible
  - it shows no children
- `Clone` is enabled only when:
  - source is a snapshot
  - target is a dataset
  - same connection
  - same pool
- Snapshots are selected from the `@` node (there is no `Select snapshot` menu anymore).
- If source or target runs OpenZFS `< 2.3.3`, `Copy`, `Level`, and `Sync` are blocked.
- `Apply changes` is enabled only when there are real property or permission drafts. Those
  two ARE edited in batches and applied with a button; actions are not: they run when you
  press them.
- Normal navigation uses cache; refresh happens explicitly or after actions that require it.
