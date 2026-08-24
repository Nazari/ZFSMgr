# Adding a command

This is a task guide. It walks one new command from nothing to something you can type,
naming every file you have to touch and, at each step, the mistake that has actually been
made there before.

It assumes you have read nothing else. If you want the wider picture afterwards, the
headers under `src/base/` and `src/commands/` carry it — they are written to be read.

## The shape of the thing

A command exists in two halves that never meet in the same process:

    a client  ──►  the agent (a daemon running as root on the target machine)

The **client** decides what to ask for and composes an argv. The **agent** receives that
argv over mTLS and runs it with `execvp`. There is no shell anywhere in between, and that
is deliberate: a dataset called `tank/a;rm -rf /` has to be a dataset name and nothing
else.

So adding a command means adding the same verb on both sides, plus telling the client's
user interface that it exists. Five places. Miss one and it fails in a way that does not
look like a missing step — the last section lists how each one fails.

## The example

We will add `--dump-zpool-history`, which answers `zpool history <pool>`. It is a read,
which keeps the example short; the differences for a mutation are called out as we go.

## 1. The argv, in `src/commands/`

Nothing in the client builds an agent argv by hand. Every verb has a function, and which
module it belongs in follows one rule:

- **It has no rule beyond its name** — it just wraps a verb and passes a couple of
  arguments — so it goes in `requests.h`.
- **It has a rule** — a flag means something, a scope maps to a letter, a name has to be
  completed — so it goes in the module that owns that rule: `pools`, `snapshots`,
  `datasets`, `advanced`, `zfsallow`.

Ours has no rule. In `src/commands/requests.h`:

```cpp
std::vector<std::string> poolHistory(const std::string& pool);
```

and in `requests.cpp`:

```cpp
std::vector<std::string> poolHistory(const std::string& pool) {
    return withOne("--dump-zpool-history", pool);
}
```

`withOne` returns an **empty vector** when the argument is empty, rather than sending the
bare verb. That is not tidiness: the agent would answer with its usage line and `rc=2`,
which is a far worse error to read than not having asked.

> **Why this module exists at all.** Measured before it was written: of 59 verbs, 34
> appeared verbatim in two or three clients. Along with the verb travels *how many
> arguments it takes and in which order*, and that was written down nowhere. When a verb
> gained an argument, the client that did not hear about it did not fail to compile — it
> failed at run time, against a machine, and with luck.

## 2. The verb in the agent — **both branches**

`src/daemon/daemon_main.cpp` dispatches the same verb in two entirely separate places:

- The **RPC branch**, which serves clients over the socket. Near line 7427 in the current
  tree; look for other `if (cmd == "--dump-...")` and add yours alongside.
- The **CLI branch**, which serves `zfsmgr_agent --dump-zpool-history tank` typed by hand.
  Near line 9416.

```cpp
// RPC branch — returns a struct
if (cmd == "--dump-zpool-history") {
    if (params.size() < 1) {
        r.rc = 2;
        r.err = std::string("usage: ") + argv0 + " --dump-zpool-history <pool>\n";
        return r;
    }
    ...
}

// CLI branch — writes to stdout and returns an exit code
if (cmd == "--dump-zpool-history") {
    if (args.size() < 3) { printUsage(args[0].c_str()); return 2; }
    return runExecStreaming("zpool", {"history", args[2]});
}
```

**This asymmetry has bitten three separate times.** Adding only the RPC branch produces an
agent that answers your client correctly and prints its usage line when a human runs the
same verb by hand — and the other way round is worse, because the client's failure looks
like a transport problem. There is no compiler check for it. Grep for your verb before you
declare it done: you should find it twice in `daemon_main.cpp`.

## 3. Declare the capability

`agentCapabilityList()` in the same file returns the list the agent publishes as `CAPS=`
in its `--health` answer. Add your verb there.

**Ask the capability, never the version.** Adding a verb does not bump the API version, so
a client cannot infer support from a version number: an agent that is "up to date" may
still not know your verb. The client reads `CAPS` and lets it override its own static
table. Skipping this step means a client offering a button that fails on click, against
an agent that is simply older.

For a platform-specific verb, push it inside the relevant `#ifdef`. Getting that wrong
in the opposite direction has also happened: transfers were left switched off on Windows
because their capabilities sat inside a `#ifndef _WIN32`.

## 4. The command in the shell

Three things, in `src/cli/`:

**The catalogue entry**, in `ayuda.cpp`. This is not documentation that happens to sit next
to the code — it *is* the signature. From it come the help text, the tab completion, the
argument checking and the syntax error messages:

```cpp
{"history", {"t_pools_2fd96d", "Pools"}, {"t_pool_destino", "[<pool>]"},
 {"t_que_se_le_ha_hecho", "What has been done to the pool, and when."}, {}, {},
 Objetivo::Pool,
 {}},
```

`Objetivo` says what kind of node the command acts on; the slots say what it takes after
that. **Anything left over is an error** — that single rule is what killed off a whole
family of bugs where a command accepted an argument and quietly ignored it.

**The handler**, in `shell.cpp`:

```cpp
bool cmdHistory(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) return false;      // resolves the target, splits the args
    const ZfsmUrl& destino = pet.objetivo;
    std::string out;
    if (!agente(e, destino, PET::poolHistory(destino.pool), out, 60000)) return false;
    ...                                             // render `out`
}
```

**The dispatch entry**, in the table near the bottom of `shell.cpp`:

```cpp
{"history", cmdHistory},
```

If your command's shape does not fit any existing grammar production, you will also need
one in `src/cli/gramatica.y`, and then to run `scripts/genera_gramatica.sh` — the
generated parser is committed, and forgetting to regenerate it means your change silently
does nothing.

> Two commands shipped in exactly that state: `job cancel <id>` and `rename` to a
> different parent were both documented, both implemented, and both unreachable, because
> no production accepted their shape. The tests in §6 exist because of them.

## 5. The messages

Every string the user sees goes through `T("key", "Spanish text")`. The Spanish is written
in the code and the key accompanies it, so a missing, incomplete or corrupt catalogue
degrades to "it comes out in Spanish" and never to "a key comes out".

What does **not** go through it, and this is not an oversight: verb names, tsv and json
field names, and URL literals. Those are an interface for programs and stay in English
always — translating them would break any script the moment somebody changed language.

## 6. The tests

`tests/gramatica_cli_test.cpp` will already check some of your work automatically:

- every command in the catalogue fits some grammar production;
- every declared option is recognised when typed;
- every example in the help parses, and its verb is the command that hosts it;
- every declared option appears in at least one example.

That last pair is why the help asks for an example per option. Add yours to the `kEjemplos`
table in `ayuda.cpp` or the tests will fail — deliberately.

For the argv builder itself, add assertions to `tests/base_test.cpp`. They are cheap, they
need no machine, and this is the layer where a wrong argv is still visible.

## How each missing step fails

| Skipped | What you see |
|---|---|
| The `commands/` function | Nothing, at first. Then a second client builds the same argv slightly differently and one of them breaks at run time. |
| The RPC branch | The client gets the agent's usage line and `rc=2`, which reads like a transport failure. |
| The CLI branch | Everything works until somebody runs the verb by hand on the machine, and it prints usage. |
| `CAPS` | The client offers the feature against agents that do not have it. Fails on click. |
| The catalogue entry | The command does not exist for the shell: no help, no completion, no argument checking. |
| The grammar production | «syntax error» on a command that is fully implemented. |
| Regenerating the parser | Your change compiles and does nothing at all. |

## The rules that are not negotiable

**Secrets never travel in argv or in the environment.** Both are readable by anyone with
`ps` or `/proc/<pid>/environ`. They go by file descriptor or by terminal. See
`helpers::SecretFromDescriptor`.

**A mutation that may have arrived is never resent.** `transportcmd::isMutatingAgentCommand`
decides which verbs count, and the transport marks the point of no return *before* writing
the first byte, because a partial write arrives too. Resending a `--dump-*` costs nothing;
resending a `--job-submit` launches the same transfer twice over the same data.

**The base layer does not translate and does not print.** It returns typed reasons —
`transport::Failure`, `store::Reason`, `gsa::Failure` — and whoever has an interface words
them. This is enforced by the build: `zfsmgr_base` links no Qt, and its include directory
is only `../src/base`, so an `#include "commands/..."` from `base/` does not compile.

**Empty output is usually not an error.** A dataset with nothing delegated, a directory
with no files, a machine with no pools: all legitimate answers. Treating them as parse
failures produced messages like "unreadable answer" on a machine where the only thing going
on was that there were no pools yet.
