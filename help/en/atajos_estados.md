# Navigation and states

- The cursor switches to busy during actions and refreshes.
- **The source is the left pane and the destination the right one.** Nothing has to be marked
  as source: it is the source by being where it is.
- Selecting a snapshot in the `Snapshots` tab counts the same as selecting it in the tree.
- Changing a pane's connection changes its tree, its detail and its log at once.
- If a connection is disconnected, its tree stays empty and says so.
- `Clone` is enabled only when:
  - source is a snapshot
  - target is a dataset
  - same connection
  - same pool
- If source or target runs OpenZFS `< 2.3.3`, `Send`, `Level`, and `Sync` are blocked.
- `Apply changes` is enabled only when there are real property or permission drafts, and the
  box next to it lists which. Those two ARE edited in batches; actions are not: they run when
  you press them.
- Normal navigation uses cache; refresh happens explicitly or after actions that require it.
- Each pane remembers separately what it had expanded: both can sit on the same connection
  without stepping on each other.
- The connection and pool choice survives refreshes: it is looked up by identifier, not by
  position in the list.
