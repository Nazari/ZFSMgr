# Action: Breakdown

Goal: create child datasets from directories in the parent dataset.

Conditions:

- Dataset selected in the pool tree.
- The dataset must be **mounted**: if it is not, no candidate directory is listed.
- On Unix connections it requires the `zfsmgr-agent` daemon to be installed: there is no shell fallback.

Behavior:

- Shows a selection dialog for directories.
- Directories that already correspond to child datasets, or that contain mountpoints of descendants, are hidden. Names that are not valid as a dataset (containing `@`, `#`, `,`, `.`, `..` or control characters) are shown but cannot be selected.
- Shows the command, asks for confirmation and runs immediately, blocking the interface while it lasts.
- For each directory: creates the child dataset mounted on a temporary directory under `/tmp`, copies the contents, removes the source directory, and only then moves the child's `mountpoint` to the original path and remounts it.
- Logs the selected directories at NORMAL level before starting, and one `[BREAKDOWN] ok` line per directory when finishing. There is no real-time progress during the copy.

Important warnings:

- **`/tmp` on the remote system is used as intermediate storage**: there must be enough free space for the contents of each directory.
- Before removing the source directory, the copy is **verified as complete** (a dry `rsync` pass counting pending files, with retries). If something is still missing after several attempts, the action stops and **removes nothing**.
- The copy preserves **ACLs and extended attributes** when the system's `rsync` supports them (detected automatically).
