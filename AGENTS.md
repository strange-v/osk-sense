# Repository agent notes

## Node.js commands on Windows

The user's npm installation is valid, but its system shim resolves the configured global prefix through `%APPDATA%\npm`. Sandboxed agent commands cannot read that directory. In PowerShell, run the system npm CLI directly instead of invoking `npm`, `npm.cmd`, or `pnpm`:

```powershell
& "$env:ProgramFiles\nodejs\node.exe" "$env:ProgramFiles\nodejs\node_modules\npm\bin\npm-cli.js" <arguments>
```

For example, use the same command with `test`, `run typecheck`, `run build`, or `ci` as its arguments. Do not switch this npm-managed workspace to pnpm.

Do not run `run typecheck` and `run build` together: `build` starts with the same `vue-tsc --noEmit`, so the pair type-checks the workspace twice. Use `typecheck` alone while iterating, and `build` when the bundle matters.

## Native C++ tests

Run them in WSL from the repository root. `pio test -e native` does not work on Windows: there is no host `g++`.

| Component | Command |
| --- | --- |
| protocol | `wsl bash protocol/scripts/run_native_tests_wsl.sh` |
| node | `wsl bash node/scripts/run_native_tests_wsl.sh` |
| gateway | `wsl bash gateway/scripts/run_native_tests_wsl.sh` |

Firmware images build on Windows with `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e <env>` from `node/` or `gateway/`.

## Pre-release data and compatibility

The frontend, gateway backend/firmware, and node firmware have not been released and there are no deployed devices whose persisted data must be preserved. Do not add data migrations, legacy readers, compatibility branches, or fallback code solely to retain data written by an earlier development version. Prefer changing formats and contracts cleanly and reinstalling, reflashing, or erasing development data when needed. Add migration or backward compatibility code only when the user explicitly asks for it.

## Documentation

Reference documents describe how the system behaves now. They are not a changelog: no "used to", no "this was added because", no comparison against an earlier version. A reader who has never seen the previous state must not be able to tell there was one. Git holds the history.

The root `README.md` is for users: what the system is, what it needs, how to get started. Developer material belongs in `CONTRIBUTING.md`; component details stay in that component's README or reference documents.

Shorter is better. Prefer a table to a paragraph and a sentence to a paragraph. Drop any sentence that only restates the one before it or that justifies a decision nobody is about to make differently — rationale earns its place only where it stops a plausible wrong turn.

The same applies to code comments, with one addition: a comment explains what the code cannot, which is usually why an obvious alternative was rejected. It never narrates what changed.

## Answering versus changing

A question about how something already behaves is a question, not a work order. Answer it from the code, cite `file:line`, and stop. Do not edit, do not add tests, do not start a dev server, and do not run the suite to answer it -- if a fix is wanted, it will be asked for.

Short signals in either direction:

- `?` or "just answer" -- reply from the code and change nothing.
- "go" or "fix it" -- carry out the work and verify it per the tiers below.
- "describe the plan first" -- propose, then wait. Worth using for anything that moves files, routes, or navigation.

The exception is a question whose answer is **not** in the code -- hardware behaviour, real-world data, how a browser actually acts. Reproduce those, because reading cannot settle them. Two examples from this repository: pairing QR photos failed for the opposite of the reason the RFM69 datasheet suggested (more pixels decode *worse*), and the signal-bar thresholds were only settled by field measurements from the installed network. Neither was answerable by reasoning.

## How much to verify

Run only tests relevant to the changed behaviour and its direct consumers. Finishing a task does not require the full suite. Run the full suite only when the user explicitly requests it or a broad change makes targeted coverage insufficient. Build only affected components when compilation or bundling needs verification.

| Change | Verification |
| --- | --- |
| Answering a question, reading code | none |
| Pure logic (`utils/`, parsers, formatting) | just that test file |
| Component or view behaviour | relevant unit and component test files |
| CSS, layout, theming | browser, with a screenshot |
| Finishing a piece of work | relevant tests once at the end; affected builds as needed |

In the browser preview:

- No `setTimeout` wait longer than about two seconds. Testing an interval or a timeout belongs in a test with fake timers, not in a live wait.
- Check `document.visibilityState` before anything timing-sensitive. A hidden pane throttles `setInterval` to roughly once a minute and starves `requestAnimationFrame`, which reads exactly like a broken feature.
- Drive state through the mock gateway (`ui/vite.mock.config.ts`) rather than waiting for real time to pass.
