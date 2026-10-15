# Repository agent notes

Read [CONTRIBUTING.md](CONTRIBUTING.md) before changing code or documentation: it holds the checks, the pre-release compatibility policy (no migrations or compatibility code unless the user explicitly asks for them) and the documentation style.

## Node.js commands on Windows

The user's npm installation is valid, but its system shim resolves the configured global prefix through `%APPDATA%\npm`. Sandboxed agent commands cannot read that directory. In PowerShell, run the system npm CLI directly instead of invoking `npm`, `npm.cmd`, or `pnpm`:

```powershell
& "$env:ProgramFiles\nodejs\node.exe" "$env:ProgramFiles\nodejs\node_modules\npm\bin\npm-cli.js" <arguments>
```

For example, use the same command with `test`, `run typecheck`, `run build`, or `ci` as its arguments. Do not switch this npm-managed workspace to pnpm.

Do not run `run typecheck` and `run build` together: `build` starts with the same `vue-tsc --noEmit`, so the pair type-checks the workspace twice. Use `typecheck` alone while iterating, and `build` when the bundle matters.

PlatformIO is not on the sandbox `PATH`; build firmware with `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e <env>` from `node/` or `gateway/`.

## Answering versus changing

A question about how something already behaves is a question, not a work order. Answer it from the code, cite `file:line`, and stop. Do not edit, do not add tests, do not start a dev server, and do not run the suite to answer it -- if a fix is wanted, it will be asked for.

Short signals in either direction:

- `?` or "just answer" -- reply from the code and change nothing.
- "go" or "fix it" -- carry out the work and verify it per the tiers below.
- "describe the plan first" -- propose, then wait. Worth using for anything that moves files, routes, or navigation.

The exception is a question whose answer is **not** in the code -- hardware behaviour, real-world data, how a browser actually acts. Reproduce those, because reading cannot settle them. Two examples from this repository: pairing QR photos failed for the opposite of the reason the RFM69 datasheet suggested (more pixels decode _worse_), and the signal-bar thresholds were only settled by field measurements from the installed network. Neither was answerable by reasoning.

## How much to verify

Run only tests relevant to the changed behaviour and its direct consumers. Finishing a task does not require the full suite. Run the full suite only when the user explicitly requests it or a broad change makes targeted coverage insufficient. Build only affected components when compilation or bundling needs verification.

| Change                                     | Verification                                              |
| ------------------------------------------ | --------------------------------------------------------- |
| Answering a question, reading code         | none                                                      |
| Pure logic (`utils/`, parsers, formatting) | just that test file                                       |
| Component or view behaviour                | relevant unit and component test files                    |
| CSS, layout, theming                       | browser, with a screenshot                                |
| Finishing a piece of work                  | relevant tests once at the end; affected builds as needed |

In the browser preview:

- No `setTimeout` wait longer than about two seconds. Testing an interval or a timeout belongs in a test with fake timers, not in a live wait.
- Check `document.visibilityState` before anything timing-sensitive. A hidden pane throttles `setInterval` to roughly once a minute and starves `requestAnimationFrame`, which reads exactly like a broken feature.
- Drive state through the mock gateway (`ui/vite.mock.config.ts`) rather than waiting for real time to pass.
