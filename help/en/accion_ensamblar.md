# Action: Assemble

Goal: convert child datasets into directories under the parent dataset.

Conditions:

- Dataset selected in the pool tree.
- The parent dataset must be **mounted**; otherwise the operation fails with `mountpoint=none`. Each selected child dataset is mounted by the daemon itself.
- On Unix connections it requires the `zfsmgr-agent` daemon to be installed: there is no shell fallback.

Behavior:

- Shows a selection dialog for child datasets. The listing is recursive: it includes nested descendants, not just direct children.
- Shows the command, asks for confirmation and runs immediately, blocking the interface while it lasts.
- For each child dataset: copies its contents to a temporary directory under `/tmp` on the remote system, destroys the child dataset (`zfs destroy -r`) and copies the contents from the temporary directory into the matching directory inside the parent.
- The child dataset is destroyed only if the copy **to the temporary directory** completed successfully.
- Logs the selected child datasets at NORMAL level before starting, and one `[ASSEMBLE] ok` line per child dataset when finishing. There is no real-time progress during the copy.

Important warnings:

- **`zfs destroy -r` also takes the descendants** of the selected child dataset with it.
- **`/tmp` on the remote system is used as intermediate storage**: there must be enough free space.
- The copy preserves **ACLs and extended attributes** when the system's `rsync` supports them (detected automatically).
- Before destroying the source dataset, the copy is **verified as complete**; if anything is missing the action stops and the dataset is **not destroyed**.
