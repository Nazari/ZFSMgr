# Action: Assemble

Goal: absorb a child dataset into its parent dataset, leaving its contents as a directory.

Requirements:

- **It IS available on Windows connections**, unlike `Breakdown`. Verified against a real
  machine: datasets go back to being directories in place.
- A dataset selected in the pool tree.
- The parent dataset must be **mounted**; otherwise the operation fails with `mountpoint=none`. Each selected child dataset is mounted by the agent itself.
- On Unix connections it requires the `zfsmgr-agent` agent: there is no shell fallback.

## Choosing what to absorb

The selection window has two columns: the directory, and **the dataset it currently is**. Here the name is a fact, not a proposal, so it is not editable.

Child datasets are chosen **independently**: absorbing one does not force absorbing the ones below it.

## What it does

- It shows the command, asks for confirmation and runs immediately, blocking the interface while it lasts.
- Contents are copied to a staging area **inside the pool itself**, and the final step is a rename rather than a second copy: the data does not travel twice.
- **Child datasets hanging from the absorbed one are preserved.** They are not destroyed: they are reassigned to the parent and stay mounted where they were, so their contents are untouched.
- On reassignment the name comes to encode **where they stay mounted**: the path under the parent with `:` instead of `/`.

```
testpool/user/bin/Squirrel.app/Contents   (before)
testpool/user/bin/Squirrel.app:Contents   (after, mounted in the same place)
```

That way the name keeps the path and a later Break down can reconstruct it. The dataset name stops mirroring the directory structure, which is expected: ZFS does not require them to match.

- The *Progress* box publishes a line every two seconds with the current item, the amount copied and the speed.

## Important warnings

- Intermediate space is taken **from the pool itself**, not from the system `/tmp`. On most Linux distributions `/tmp` lives in memory, and using it made assembling a large dataset impossible.
- Before destroying the source dataset the copy is **verified to be complete**; if something is missing, the action stops and the dataset **is not destroyed**.
- **Only** the chosen dataset is destroyed, without `-r`: its descendants have already been reassigned.
- The copy preserves **ACLs and extended attributes** when the system `rsync` supports them (detected automatically).
