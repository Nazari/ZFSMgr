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
- Copies the contents with `tar --acls --xattrs` on Unix connections, preserving ACLs and extended attributes. On Windows connections `tar` is used without ACLs or xattrs.
- If the copy completes successfully and the matching checkbox was ticked, removes the source directories.
- The action is queued in `Pending changes` and runs only when changes are applied.
- Commands and results are logged in the combined log.

Important warnings:

- **The dataset is left mounted when finished**, also when you choose not to remove the source directories.
- There is no post-copy verification: removal of the source depends solely on the copy's exit code.
