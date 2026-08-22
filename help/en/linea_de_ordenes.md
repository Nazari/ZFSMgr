# Command line (`zfsmgr-cli`)

ZFSMgr ships a terminal tool that does what the window does, against the **same
connections**: it reads `config.json` and `trust-store.json` from the same place, talks
over the same encrypted tunnel and runs the same agent verbs. It is not a separate
program with its own configuration.

On Windows the installer puts it on the `PATH` — if you leave that box ticked — so you
call it by name from `cmd` or PowerShell. On Linux and macOS it **is installed
alongside the application**, in `bin`, so it is also called by name. That was not the case
before, and it did not ship in any Unix package.

There is also a **web server**, `zfsmgr-web`: it shows the same thing in a browser, without
JavaScript, reached over an SSH tunnel. All three — window, shell and server — talk to the
same agent and share the same rules.

## Two ways to use it

**With a command**, for scripts:

```sh
zfsmgr-cli connections list
zfsmgr-cli --format json connections list | jq '.connections[] | select(.tls == false)'
```

**With no command at all**, and then it behaves as a shell: there is a location — a
`zfsm://` URL — and everything you type acts on it.

```text
zfsm://local> cd oldlau/winpool
zfsm://oldlau/winpool> ls
zfsm://oldlau/winpool> cd sa@yesterday
zfsm://oldlau/winpool/sa@yesterday> ls #content
```

`help` lists the commands and `help <command>` explains one. Tab completes commands and
URLs; the arrows walk the history.

## The location is a URL

The same one the application uses: `zfsm://connection/pool/dataset@snapshot#section`.
The sections are `#content[/path]` (the files inside), `#properties[/prop]` and
`#permissions`, and they are **in English** like the rest of the URL.

That has a practical consequence: **any command can be copied from the history and run
on its own**, by adding `--on <url>` (or `--from`, which is the same). Commands that
need a source and a destination take the current location as the source unless told
otherwise.

Two rules disambiguate a relative path:

- If the first segment names a **connection**, the path is absolute. That is what you
  type when hopping between machines: `cd unibody` from `zfsm://local`.
- If the first segment is the **pool you are already in**, it is taken as the full ZFS
  name and not as a child. Standing in `tank/source`, `destroy tank/clone` points at
  `tank/clone`, not at `tank/source/tank/clone`.

To descend into a child named like a connection, use `./name`.

## The three output formats

- `text` (the default) is for reading: aligned columns, translated headers, human-sized
  numbers and booleans as `yes`/`no`.
- `tsv` is for scripts: no header, tab-separated, fixed columns **in English** and `-`
  where there is no value, just like `zfs list -H`.
- `json` is for programs: numbers come out as numbers, booleans as booleans and what
  does not apply as `null`.

**In `tsv` and `json` the field names are always in English and do not change with the
language.** That is deliberate: a script should not break because someone switched the
interface language.

## Language

`--lang es|en|zh`. Without it, the one the graphical interface uses (`app.language` in
`config.json`), so both tools speak alike.

What gets translated: messages, the help, and the headers in `text` format. What does
**not**, and it is not an oversight: the verbs you type, the field names of `tsv` and
`json`, and the URL literals.

## Passwords

Never through an argument or an environment variable: both are visible in `ps` to any
user of the machine. Only through the terminal or a file descriptor, which lets you use
any secret manager:

```sh
zfsmgr-cli --password-fd 3 connections list  3< <(pass show zfsmgr)
```

With `--no-secrets` nothing is decrypted and no master password is asked for: encrypted
fields come out as `<cifrado>`. It is for taking inventory without having the secret at
hand. With that option **the configuration is not written**: saving a profile whose
encrypted fields arrived empty would leave them empty in the file, and that wipes the
stored password.

## Destructive actions

Each one asks for confirmation, listing what it will take with it. `-y` treats them as
confirmed; with no terminal and no `-y`, the command refuses to go on instead of
assuming a yes.

## When something goes wrong

`-v` reports on standard error what the transport does with each machine: which command
is sent, whether it goes over the daemon tunnel (`[daemon-rpc]`) or over SSH, and why it
failed if it did. Passwords and TLS material are masked in that log.

What you asked for goes to **standard output** and the log to **standard error**, so
`zfsmgr-cli ... > data.tsv` does not mix the two.
