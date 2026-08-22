# Action: From Dir

Goal: create a child dataset from one or more existing directories.

Conditions:

- Dataset selected in the pool tree.
- Selection must be a dataset (not a snapshot).
- On Unix connections it requires the `zfsmgr-agent` daemon to be installed: there is no shell fallback.

Behavior:

- Opens a dialog to define the new dataset (properties, parent creation with `-p` and optional encryption) and to pick the source directories.
- You can pick **several directories, even from different connections**. If you pick a directory that already contains another picked one, the descendant is discarded. The relative hierarchy of each directory is reproduced inside the destination dataset.
- Creates the dataset, forces `canmount=on` and mounts it on its final mountpoint.
- **Data goes machine to machine, not through yours.** When both ends have the agent, the
  destination listens and the source sends it the tree directly. Copying 100 GB from one
  machine to another no longer moves 200 GB through the machine you are driving from.
- It is a **daemon job**: there is progress, it can be cancelled, and it survives closing
  the window.
- The copy is **incremental**: a second pass only moves what changed.
- If either end lacks the agent, or if you tick the box to delete the source directories,
  the old path is used: a `tar --acls --xattrs` pipeline with both ends over SSH, which does
  push the data through your machine. The deletion goes that way on purpose: on the new path
  the copy is asynchronous, so the deletion would fire without knowing whether it finished.
- If the copy completes successfully and the matching checkbox was ticked, removes the source directories.
- **It runs when you press it.** It used to be added to a pending-changes list and wait for
  you to apply it; that list is gone.
- Commands and results are logged in the combined log.

Important warnings:

- **The dataset is left mounted when finished**, also when you choose not to remove the source directories.
- There is no post-copy verification: removal of the source depends solely on the copy's exit code.
