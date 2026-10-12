# Contributing to ZFSMgr

## Language

**Code is written in English**: identifiers, comments, commit messages, log lines and the
agent's messages. Text a user reads goes through the translation catalogs (`i18n/*.json`) and
is shown in the user's language.

Much of the existing code still has Spanish identifiers and comments. It is **not** translated
in one go —that would be a huge diff that hides real changes and ruins `git blame`—: **what you
touch, you translate**. A function you modify gets English names and comments; a file you
split or move is a good moment to translate it whole.

## Layout

| Path | What lives there |
|---|---|
| `src/base`, `src/commands` | The core: rules as pure functions with tests, typed results, no Qt. Clients and the agent link it. |
| `src/daemon` | The agent (`zfsmgr-agent`). `verbs.cpp` holds the ONE verb table both its doors use (RPC and command line). |
| `src/cli` | The command line client (`zfsmgr-cli`). |
| `src/native` | The Qt interface, frozen until 2.0. |
| `tests` | Unit tests. `scripts/prueba_cli.py` is the end-to-end battery against real machines. |

Rules belong in the core, not in a client: if two clients would need the same decision, it
goes in `src/base` or `src/commands`, with a test.

## Style

- **Formatting** is `.clang-format`. Only the lines a change touches are checked, so old code
  is reformatted as it is touched, never in bulk. Before committing:
  `scripts/formato.sh` (formats what you changed) or `scripts/formato.sh --check HEAD~1`.
  The CI runs the same check on every push.
- **Functions stay short.** A function that grows past a screen or two is split by what its
  pieces do (one per phase of a protocol, one per kind of operation). A class gathers state
  that several of those functions share.
- **Comments say why**, in a few lines: the reason, the trap, what would break. The history of
  how something was found («it used to…», «measured on…») goes in the commit message or in
  `docs/`, not in the code, where it goes stale.
- **Errors are typed.** The agent answers with a code from `src/base/agentmsg` and its
  arguments; the client words it. No decisions by comparing message strings.
- **Secrets never travel in argv or the environment**: only through a file descriptor or
  standard input.

## Testing

- `cmake --build builds/linux && (cd builds/linux && ctest)`.
- `./scripts/buildall-cross.sh` compiles every platform (Linux, Windows, macOS, FreeBSD). The
  GitHub CI also builds with MSVC, which catches things MinGW lets through.
- The battery, `scripts/prueba_cli.py`, for anything the CLI or the agent does.
- A new test should fail when the code it covers is broken: break it once and watch.
