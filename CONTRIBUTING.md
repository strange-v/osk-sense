# Contributing

## Pre-release compatibility

The frontend, gateway firmware and node firmware have not been released to deployed devices whose persisted data must be preserved. Change formats and contracts cleanly and reinstall, reflash or erase development data when needed. Do not add data migrations, legacy readers, compatibility branches or fallback code solely to keep data written by an earlier development version, unless that is an explicit decision.

## Checks

From the repository root:

```powershell
node protocol/scripts/validate_protocol_artifacts.mjs
node protocol/scripts/generate_protocol_docs.mjs
wsl bash protocol/scripts/run_native_tests_wsl.sh
wsl bash node/scripts/run_native_tests_wsl.sh
wsl bash gateway/scripts/run_native_tests_wsl.sh
```

Web UI tests and build are in the [gateway README](gateway/README.md#web-ui-filesystem).

Native C++ tests run in WSL: `pio test -e native` does not work on Windows, which has no host `g++`. Firmware images build on Windows with `pio run -e <env>` from `node/` or `gateway/`.

Run only the affected checks. Protocol and node scripts accept suite names after
the optional `--sanitize` flag, for example `--sanitize test_security_service`
for the node. The gateway script accepts `--backup` for backup, restore and reset
checks, or `--registry-radio` for registry/radio storage checks. Omitting selectors
runs all tests in that script.

## Documentation

Reference documents describe how the system behaves now. They are not a changelog: no "used to", no "this was added because", no comparison against an earlier version. A reader who has never seen the previous state must not be able to tell there was one. Git holds the history. [ARCHITECTURE.md](ARCHITECTURE.md#documentation-ownership) names the document for each topic.

The root `README.md` is for users: what the system is, what it needs, how to get started. Developer material belongs here; component details stay in that component's README or reference documents.

Shorter is better. Prefer a table to a paragraph and a sentence to a paragraph. Drop any sentence that only restates the one before it or that justifies a decision nobody is about to make differently — rationale earns its place only where it stops a plausible wrong turn.

The same applies to code comments, with one addition: a comment explains what the code cannot, which is usually why an obvious alternative was rejected. It never narrates what changed.

## Formatting

Install the formatters and enable the pre-commit hook once per clone (Node.js 24):

```powershell
& "$env:ProgramFiles\nodejs\node.exe" "$env:ProgramFiles\nodejs\node_modules\npm\bin\npm-cli.js" --prefix gateway/ui ci
git config core.hooksPath .githooks
```

Open the repository root in VS Code and install the recommended Prettier and
Clang-Format extensions. Workspace settings enable formatting on save using the
local, pinned formatters. On Linux or macOS, use the bundled
`gateway/ui/node_modules/.bin/clang-format` executable; Windows uses the native
binary directly. On Linux or macOS, also run `chmod +x .githooks/pre-commit`.

| Files                          | Formatter    | Style                                                           |
| ------------------------------ | ------------ | --------------------------------------------------------------- |
| C/C++                          | clang-format | 4 spaces, 100 columns, expanded blocks                          |
| Web code, JSON, Markdown, YAML | Prettier     | 2 spaces, 100 columns, single quotes and no semicolons in JS/TS |

In C++, simple single-statement conditions have no braces and place the body on
the next line. Separate logical stages with blank lines. clang-format preserves
braces when removing them would make control flow ambiguous. Generated files, package locks,
dependencies and firmware filesystem assets are excluded from the commit hook.

The hook formats staged files, stages the formatted result and restores unstaged
edits in partially staged files. Formatter failures stop the commit and restore
the index and working files. Review the result in `git show` after committing.

Manual commands from the repository root:

```powershell
node gateway/ui/scripts/format.mjs --write gateway/src/CommandService.cpp
node gateway/ui/scripts/format.mjs --check gateway/ui/src/components/NodeCommands.vue
node gateway/ui/scripts/format.mjs --staged
```

From `gateway/ui`, `npm run format` formats all tracked supported files in the
repository; `npm run format:check` checks them without writing. On Windows, use
the npm CLI invocation above with `run format` or `run format:check` instead of
`ci`.

The hook also validates changed protocol artifacts. It regenerates diagrams only
when the staged manifest or diagram generator changed, and stops the commit if
the generated SVG files need to be staged.
