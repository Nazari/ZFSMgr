# Action: Break down

Goal: turn directories of the dataset into child datasets.

Requirements:

- **Not available on Windows connections.** There, datasets mount under the pool's drive
  letter and cannot have their own mountpoint, so the new dataset would not end up where
  the directory is: its files would appear at the root of the drive. The action is shown
  disabled with that reason. See `Windows connections`.
- A dataset selected in the pool tree.
- The dataset must be **mounted**: otherwise no candidate directory is listed.
- On Unix connections it requires the `zfsmgr-agent` agent: there is no shell fallback.

## Choosing what to convert, and under which name

The selection window has two columns: the directory, and **the dataset it will become**, which is editable.

**The directories above it do not have to be converted too.** `zfs create` only requires the *parent dataset by name* to exist; the mountpoint path is irrelevant to it. So `a/b/c` can be converted while `a` and `b` stay plain directories.

The proposed name reflects that decision: segments that are also being converted are separated with `/`, the ones that are not with `:`.

```
selecting only a/b/c   ->  <dataset>/a:b:c
selecting a, b and c   ->  <dataset>/a/b/c
```

The column is recomputed as you check and uncheck, except on rows whose name you edited yourself. ZFS only accepts `a-z A-Z 0-9 _ . : -` and space; `:` is one of the few legal ones that hardly ever appears in directory names, which is why it marks "this segment is not a dataset". An invalid, duplicated or already taken name is shown in red with the reason and blocks the Accept button.

Directories that already correspond to child datasets under their canonical name are hidden. Those shown only to place others are disabled. Names that are invalid as a dataset (containing `@`, `#`, `,`, `..` or control characters) are marked as not selectable.

## What it does

- It shows the command, asks for confirmation and runs immediately, blocking the interface while it lasts.
- Every byte moves **once**: each directory is first copied to a dataset under a flat, temporary name, excluding the subdirectories that will be converted on their own; only when **all** copies are verified are the originals deleted and the datasets renamed to their final name.
- A directory that **already is** the mountpoint of a dataset under another name — what an Assemble that preserved child datasets leaves behind — is not copied: it is only renamed.
- If there are child datasets mounted inside a directory being converted, they are unmounted before anything is deleted and mounted again at the end.
- The *Progress* box publishes a line every two seconds with the current item, the amount copied and the speed:

```
[BREAKDOWN] 8 of 13: Disks/Bootables/Tools — 643.2 MiB copied at 64.1 MiB/s
```

## Important warnings

- Intermediate space is taken **from the pool itself**, where the data has to end up; not from the system `/tmp`. The final step is a rename, not a second copy.
- Before deleting the source directory the copy is **verified to be complete** (an `rsync` dry run counting pending files, with retries). If something is still missing after several attempts, the action stops and **deletes nothing**.
- Deletion of the original **does not cross mount points**: if it found something mounted that had not been unmounted first, it stops instead of deleting its contents.
- If the final rename fails, the data is intact in the temporary datasets and the message states which they are and which name each should get. **Do not destroy them.**
- The copy preserves **ACLs and extended attributes** when the system `rsync` supports them (detected automatically).
