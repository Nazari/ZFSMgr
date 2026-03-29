# Navigation and states

- The cursor switches to busy during actions and refreshes.
- The unified tree is now the main navigation surface.
- The current tree selection does not replace logical `Source` and `Target`.
- `Source` and `Target` are set explicitly from the dataset context menu.
- The `Selected datasets` box reflects that logical selection.
- If a connection is disconnected:
  - the connection root stays visible
  - its pools disappear
- `Clone` is enabled only when:
  - source is a snapshot
  - target is a dataset
  - same connection
  - same pool
- If source or target runs OpenZFS `< 2.3.3`, `Copy`, `Level`, and `Sync` are blocked.
- `Apply changes` is enabled only when `Pending changes` contains real work.
- Normal navigation uses cache; refresh happens explicitly or after actions that require it.
