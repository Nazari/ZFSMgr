# Action: To Dir

Goal: copy dataset content into a directory on the selected connection host.

Conditions:

- Dataset selected in the pool tree.
- Selection must be a dataset (not a snapshot).
- On Unix connections it requires the `zfsmgr-agent` daemon to be installed: there is no shell fallback.

Behavior:

- Opens a dialog to choose the destination directory.
- The action is added to `Pending changes` and only runs when you apply the changes. On applying it shows the command, asks for confirmation and blocks the interface while it lasts.
- Relocates the dataset onto a temporary mountpoint and copies its contents with `rsync` (permissions, hard links, sparse files and, where the system supports them, ACLs and extended attributes). Relocating it is what allows the destination directory to be the very path where the dataset was mounted.
- If the copy completes successfully:
  - optionally deletes the source dataset (checkbox).
- Logs the command and the result (`[TODIR] ok`) in the combined log.

Important warnings:

- **Contents are NOT merged**: they are copied to a staging area first and swapped in at the end, so the destination directory ends up holding either the previous contents or the new ones, never a mixture. The previous contents are moved aside as a backup and restored if anything fails.
- A destination directory that is already a ZFS mountpoint is **rejected**.
- The copy preserves **ACLs and extended attributes** when the system's `rsync` supports them (detected automatically).
- **If the dataset is not deleted it stays mounted** at its usual mountpoint. Check that it does not overlap the destination directory.
- There is no post-copy verification: the outcome is decided by the `rsync` exit code.
- There are no real-time progress lines: output appears when the operation finishes.
