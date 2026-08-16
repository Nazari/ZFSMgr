# Action: To Dir

Goal: copy dataset content into a directory on the selected connection host.

Conditions:

- Dataset selected in the pool tree.
- Selection must be a dataset (not a snapshot).
- On Unix connections it requires the `zfsmgr-agent` daemon to be installed: there is no shell fallback.

Behavior:

- Opens a dialog to choose the destination directory.
- The action is added to `Pending changes` and only runs when you apply the changes. On applying it shows the command, asks for confirmation and blocks the interface while it lasts.
- Relocates the dataset onto a temporary mountpoint and copies its contents with the agent's own copy (permissions, hard links, sparse files, extended attributes and, with them, POSIX ACLs). Relocating it is what allows the destination directory to be the very path where the dataset was mounted.
- If the copy completes successfully:
  - optionally deletes the source dataset (checkbox).
- Logs the command and the result (`[TODIR] ok`) in the combined log.

Important warnings:

- **Contents are NOT merged**: they are copied to a staging area first and swapped in at the end, so the destination directory ends up holding either the previous contents or the new ones, never a mixture. The previous contents are moved aside as a backup and restored if anything fails.
- A destination directory that is already a ZFS mountpoint is **rejected**.
- The copy preserves **extended attributes and POSIX ACLs** on Linux and macOS. Not on FreeBSD: it uses a different attribute interface and that is not ported.
- **If the dataset is not deleted it stays mounted** at its usual mountpoint. Check that it does not overlap the destination directory.
- **There IS verification before anything is deleted**: once the copy finishes, what is still missing at the destination is counted and retried up to four times; if anything is still pending, the action stops with `verify_failed` and does not destroy the dataset.
- There are no real-time progress lines: output appears when the operation finishes.
