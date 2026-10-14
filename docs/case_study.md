# My data with ZFSMgr: a case study

*Versión en castellano: [caso_practico.md](caso_practico.md).*

A server running Docker, a backup machine and a client laptop: how to manage and protect all of it with ZFSMgr, step by step, with three machines: linux1, linux2 and macos1.

## The setup

```
            macos1 (client, no OpenZFS)
            zfsmgr-cli / ZFSMgr window
               │                     │
          SSH + TLS              SSH + TLS
               ▼                     ▼
   ┌───────────────────────┐   ┌───────────────────────┐
   │ linux1 (server)       │   │ linux2 (backup)       │
   │ pool tank · 1.8 TB    │   │ pool backup · 2.5 TB  │
   │ Docker and services   │   │ replica: backup/data  │
   │ zfsmgr-agent          │──▶│ zfsmgr-agent          │
   └───────────────────────┘   └───────────────────────┘
        nightly leveling daemon → daemon (mTLS, :47653)
```

linux1 holds the data and runs the services; linux2 holds the replica, which linux1's daemon keeps up to date with nobody at the keyboard; macos1, without OpenZFS, only drives. Both machines with data run `zfsmgr-agent`, and the client always talks to them through it: it never runs a shell on them.

## 1. Adding each machine

Everything is done from macos1: that is where the client lives (`zfsmgr-cli` or the ZFSMgr window), and from there linux1 and linux2 are driven over SSH. The first time it starts, it asks for a **master password**: it encrypts on disk the passwords and certificates of every machine.

**Step 1 — the connections.** At the root (`zfsm://`), `create` creates a connection. The sudo password never goes on the command line: it is typed in or read from a file descriptor.

```
create linux1 --type SSH --host linux1 --user admin --sudo --os linux
create linux2 --type SSH --host linux2 --user admin --sudo --os linux
ls
```

`ls` at the root shows the connections; `cd linux1` enters one and `ls` shows its pools. In the window it is the same: **New connection** in the connections menu.

**Step 2 — the daemon on each machine.** The work is done by `zfsmgr-agent`, a service that runs as root on every machine with OpenZFS and only speaks TLS with pinned certificates. A single command installs it where it is missing and updates it where it has fallen behind:

```
install-daemon -a --check     # what it would do, touching nothing
install-daemon -a
```

From here on no command runs a shell on the machines: everything is a typed daemon verb.

**Step 3 — letting the daemons talk to each other.** Scheduled snapshots are leveled at night, with the laptop off: linux1's daemon must be able to reach linux2's on its own. Two things are needed:

```
peers --listen 0.0.0.0 --on zfsm://linux2   # linux2 RECEIVES: its daemon listens on the network
peers --push --on zfsm://linux1             # linux1 gets the credentials of the others
peers --on zfsm://linux1                    # check: it must list linux2
```

`--listen` restarts linux2's daemon so it listens on the network, protected by mTLS. If linux2 should only be reachable from a given network (a VPN, the LAN), the limit goes in its firewall. `--push` asks before sending anything: it hands over private keys, which end up in a root-owned file with mode 600.

macos1, without OpenZFS, needs neither a daemon nor `--listen`: it is only the client.

## 2. Organizing the data in datasets

The rule: **one dataset per thing that is protected or moved differently.** A dataset is the unit of snapshots, copies, quotas and properties; a directory is not. Docker containers are disposable and live on `/` (ext4); what matters lives in ZFS datasets that the services mount with `bind`.

| Dataset on linux1 (pool `tank`) | Size | What it holds | Used by |
| --- | --- | --- | --- |
| `data/srvs/Mail` | 98 GB | Mail for the own domains, in Maildir | Dovecot and Roundcube |
| `data/srvs/Archive` | 84 GB | Published historical documents | Nextcloud (its data directory) |
| `data/srvs/compose` | 2 GB | The `docker-compose.yml` files and each service's configuration | Docker |
| `data/git` | 2.8 GB | The repositories | git |
| `data/Media/Music` | 12 GB | Music | Navidrome (read-only) |
| `data/Media/Movies`, `Shows` | 211 GB | Video | Jellyfin (read-only) |
| `data/Media/Books` | 7.6 GB | The Calibre library | Calibre-web, Kavita |
| `data/Media/Games` | 745 GB | ROMs and emulators | Samba |

Creating one is `create` from its parent, with its properties if needed:

```
cd linux1/tank/data/srvs
create Mail compression=zstd atime=off
ls #properties
```

`atime=off` on what is read a lot (mail, media) stops reads from counting as writes: it matters for snapshots, because a dataset that has only been read has not changed. To look inside without leaving the CLI: `ls #content` lists the dataset's files, and `cd #content/2023` enters a directory.

## 3. ROMs: one dataset per console (breakdown)

Today `Media/Games` is a single 745 GB dataset, and inside it `Roms/` has 69 directories: one per console (`3ds`, `gba`, `dreamcast`, `cps2`…). With everything in one dataset you cannot snapshot only what changes, nor give one console its own quota or compression. `breakdown` turns a directory into a child dataset **in the same place**: the emulators and Samba keep seeing the same paths.

**First, look without touching anything.** `--list` shows the candidate directories with their size and file count:

```
cd linux1/tank/data/Media/Games
breakdown --list --depth 2
```

**Then, in two steps.** First `Roms` becomes a dataset; then, inside it, each console. Arguments come in pairs: directory and name of the new dataset.

```
breakdown Roms Roms
cd Roms
breakdown gba gba snes snes psx psx dreamcast dreamcast
jobs
```

Each `breakdown` is a **job** of linux1's daemon: the prompt stays free and `jobs` shows the progress. Every byte is moved only once, and the originals are removed at the end, once all copies are verified: if something fails half-way, what was there before is intact. With 745 GB it is better to go in batches of consoles, not all 69 at once.

**In the window:** right-click on the dataset → *Breakdown*. The dialog shows the directory tree with sizes; tick the ones to convert and it runs the same command.

Once split, each console can have its own settings: for example `set compression=zstd-19` on the ones that no longer change, or snapshots only of the ones that do, without dragging the whole 745 GB along.

## 4. Books: from one dataset per author to a single one (assemble)

The opposite case. `Media/Books/CalibreLibrary` has 2,791 authors; if each were a dataset, that would be 2,791 mount points, 2,791 snapshot series and an endless `zfs list`, for 7.6 GB that are managed together. Calibre treats them as folders of a library, and that is what they should be. `assemble` turns each child dataset back into a plain directory **in the same place**: Calibre-web and Kavita notice nothing.

```
cd linux1/tank/data/Media/Books/CalibreLibrary
ls
assemble "A. A. Milne" "A. S. Byatt" "Agatha Christie"
```

Names are relative to the current dataset, and quoted when they contain spaces. `assemble` takes no wildcards, so with thousands of authors there are two ways:

- **The window:** right-click on `CalibreLibrary` → *Assemble*, **Select all** and accept. This is the comfortable way for this case.
- **The CLI from a script:** list the children with `--format tsv` and pass them to `assemble` in batches.

Like `breakdown`, it is a daemon job: follow it with `jobs`, and if something is cut half-way, the original is intact. Before starting, a snapshot of the parent is cheap insurance: `create @before-assemble -r`.

## 5. Scheduled snapshots and automatic copy to linux2

A schedule says how many snapshots of each kind are kept and, optionally, which dataset on another machine they are **leveled** to after each one. linux1's daemon runs it every five minutes, with the laptop off; the configuration is dataset properties (`org.zfsmgr:schedule-*`), so it travels with the pool.

The first two, for what matters most:

```
cd linux1/tank/data/srvs
schedule --daily 7 --recursive --to zfsm://linux2/backup/data/srvs --level
cd ../git
schedule --daily 7 --to zfsm://linux2/backup/data/git --level
```

`--recursive` on `srvs` covers `Mail`, `Archive` and `compose` at once, with one atomic snapshot of all three. Leveling leaves on linux2 the same snapshots as on linux1: it sends only what is new since the last common one (found by guid) and prunes on linux2 what linux1 no longer keeps.

A proposal for the rest, based on how much each thing changes:

| Dataset | Retention | Levels to linux2 | Why |
| --- | --- | --- | --- |
| `data/srvs` (recursive) | 24 hourly, 7 daily, 4 weekly, 12 monthly | yes | Mail and documents: what cannot be obtained again |
| `data/git` | 24 hourly, 7 daily | yes | Changes often and weighs little |
| `Media/Music`, `Books`, `Movies`, `Shows` (each) | 4 weekly, 6 monthly | yes | Rarely change; the risk is deleting by mistake |
| `Media/Games` (recursive) | 2 monthly | no | 745 GB that can be obtained again |

Two schedules in the same pool cannot overlap if one is recursive: that is why `Media` has none of its own and each child has its own. `schedule` checks this before writing anything.

If the dataset has not changed since the last snapshot of a kind, no new one is taken: the previous one is renamed. That is why `atime=off` matters for media.

**Watch out for databases.** Nextcloud keeps its files in `srvs/Archive`, but its PostgreSQL lives in a Docker volume on `/`, which is ext4 and outside the snapshots. For a consistent restore, schedule a periodic dump of the database inside `srvs`, or move that volume to a dataset.

**In the window:** the dataset's *Schedules* tab, which runs the same `schedule` lines.

## 6. Day to day: knowing everything is fine

When `zfsmgr-cli` opens on macos1, the bottom line already answers the three daily questions, without typing anything:

```
zfsm://linux1/tank/data>
next: 00:00 linux1/tank/data/git · no jobs running
```

- **When the next snapshot is due** and for which dataset. If something failed, it is preceded by «schedules with errors: N» and the details are printed once at startup.
- **Which jobs are running** (a leveling, a `breakdown`), with their progress.
- **Whether any daemon is outdated** compared with the client: «outdated daemon: linux2».

For the details:

| Question | Command |
| --- | --- |
| How did the last run of each schedule go? | `schedule status -a` |
| What is happening right now on every machine? | `jobs -a --detail` |
| What failed this week? | `jobs -a --failed` |
| How is this job going? Should I stop it? | `job <id>`, `job cancel <id>` |
| What did linux1's daemon do last night? | `log --on zfsm://linux1 --lines 500` |
| Are the disks healthy? | `status` on the pool |

This is what `schedule status -a` looks like:

```
CONNECTION  DATASET         LAST SNAPSHOT              LEVEL                DESTINATION                       STATE
linux1      tank/data/git   GSA-daily-20261014-000418  ok 2026-10-14 11:30  zfsm://linux2/backup/data/git     green
linux1      tank/data/srvs  GSA-daily-20261014-000418  ok 2026-10-14 00:04  zfsm://linux2/backup/data/srvs    green
```

A red schedule says which step failed (snapshot, prune or leveling), when and why. For example, «cannot reach the daemon of linux2» if linux2 was off: nothing needs doing, because pruning never removes the common base and the next run catches up on its own. To force it without waiting: `schedule run --wait` on the dataset.

**In the window:** the dataset's *Schedules* tab (with *Run now*), the *Jobs* box and *Connections → Daemon log* show the same.

## 7. Recovering: from one file to a whole machine

Three scenes, from the most frequent to the worst.

**A mail deleted by mistake.** Last night's snapshot has it. Look inside without mounting anything and copy back only what is needed:

```
cd linux1/tank/data/srvs/Mail@GSA-daily-20261014-000418
ls #content
cd #content/<mailbox-folder>
cp zfsm://linux1/tank/data/srvs/Mail@GSA-daily-20261014-000418#content/<mailbox-folder> zfsm://linux1/tank/data/srvs/Mail#content/recovered
```

`cp` is a daemon job and keeps owners, permissions, timestamps and extended attributes. With `--check` it shows what it would do, touching nothing.

**A Nextcloud upgrade that breaks everything.** Two options. `clone` makes a copy of yesterday's snapshot alongside, to inspect it without touching the current state. `rollback` returns the whole dataset to that snapshot, discarding everything after it (stop the container first):

```
cd linux1/tank/data/srvs/Archive@GSA-daily-20261013-093056
clone tank/data/srvs/Archive-yesterday    # a copy to inspect, alongside
rollback                                  # or go fully back to that state
```

The `clone` gets a full name on purpose: with a bare name it would hang from `Archive` and show up inside Nextcloud's data.

**linux1's disk dies.** Everything scheduled is on linux2, in `backup/data`, with the same snapshot history. With the new disk and the `tank` pool created again, send it back from linux2:

```
cd linux2/backup/data/srvs
send zfsm://linux1/tank/data/srvs --from @GSA-daily-20261014-000418 -R
jobs -a
```

`-R` carries the dataset with its children and all their snapshots. If the transfer is cut, `send … --resume` continues where it stopped, without starting over. Then set the schedule again with `schedule`, and the first leveling finds the common base by guid: it does not send everything again.

**In the window:** a snapshot's content is browsed in its tree node, and *Clone*, *Rollback* and *Send here from* are in the context menu.

## 8. Maintenance

| When | What | Command |
| --- | --- | --- |
| After installing a new client version on macos1 | Update the daemons, and the `zfsmgr-cli` of the machines that have it | `install-daemon -a --check`, then `install-daemon -a` |
| Once a month | Read the whole pool and verify checksums: catches a disk starting to fail before anything is lost | `scrub` on `linux1/tank` and on `linux2/backup` |
| If a pool's `status` does not say ONLINE | See which device fails and its errors | `status`, and `clear` once it is solved |
| If a leveling was cut | Continue it, or discard what was half-received | `send … --resume` or `send … --discard-partial` |
| If a `breakdown` or `assemble` was cut | Return the dataset to its mount point | `repair-mounts`, and `--apply` fixes it |

`install-daemon -a` does not reinstall a daemon with jobs in progress: it waits until no leveling is running, or says so. The status zone warns about outdated daemons, so there is nothing to remember.

Before any big operation (a `breakdown` of hundreds of GB, a Nextcloud upgrade), a manual snapshot is free and kept apart from the scheduled ones: `create @before-<whatever> -r`. Automatic pruning does not touch manual snapshots.

## 9. Cheat sheet

The window runs the same CLI commands underneath, so what works in one works in the other. `help <command>` gives the details and examples of each.

| To | Command | In the window |
| --- | --- | --- |
| Add a machine | `create linux1 --type SSH --host linux1 --user admin --sudo` | Connections → New connection |
| Install or update the daemons | `install-daemon -a` | Connections → Reinstall/Update daemon |
| Let a machine receive levelings | `peers --listen 0.0.0.0 --on zfsm://linux2` | — |
| Give linux1 the credentials of the others | `peers --push --on zfsm://linux1` | Connections → Hand over the credentials of the other machines… |
| Create a dataset | `create Mail compression=zstd atime=off` | Create dataset |
| See the files of a dataset or a snapshot | `ls #content` | Content node in the tree |
| Directory → dataset | `breakdown gba gba snes snes` | Breakdown |
| Child datasets → directories | `assemble "A. A. Milne"` | Assemble |
| Schedule snapshots and a copy to linux2 | `schedule --daily 7 --to zfsm://linux2/backup/data/git --level` | Schedules tab |
| Schedule status | `schedule status -a` | Schedules tab |
| Run a dataset's pass now | `schedule run --wait` | Schedules → Run now |
| See and stop jobs | `jobs -a`, `job cancel <id>` | Jobs box |
| Daemon log | `log --on zfsm://linux1` | Connections → Daemon log |
| Recover files from a snapshot | `cp …@snap#content/x …#content/recovered` | — |
| A copy of a snapshot to inspect | `clone tank/data/srvs/Archive-yesterday` | Clone |
| Go back to a snapshot | `rollback` | Rollback |
| Send a dataset to another machine | `send zfsm://linux1/tank/data/srvs -R` | Send here from…, with descendants (-R) |
| Check the disks | `scrub`, `status` | Pool menu |

Cells with «—» are the ones only the CLI has today.

The window labels are those of the Spanish interface, translated; the window is not published yet and its English labels may differ.
