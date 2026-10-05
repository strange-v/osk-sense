# Development checks

From the repository root:

```powershell
node protocol/scripts/validate_protocol_artifacts.mjs
node protocol/scripts/generate_protocol_docs.mjs
wsl bash protocol/scripts/run_native_tests_wsl.sh
wsl bash node/scripts/run_native_tests_wsl.sh
wsl bash gateway/scripts/run_native_tests_wsl.sh
```

Web UI tests and build are in the [gateway README](gateway/README.md#web-ui-filesystem).

Run only the affected checks. Protocol and node scripts accept suite names after
the optional `--sanitize` flag, for example `--sanitize test_security_service`
for the node. The gateway script accepts `--backup` for backup, restore and reset
checks, or `--registry-radio` for registry/radio storage checks. Omitting selectors
runs all tests in that script.

Enable the tracked pre-commit hook once per clone:

```powershell
git config core.hooksPath .githooks
```

The hook validates changed protocol artifacts. It regenerates diagrams only when the staged manifest or diagram generator changed, and stops the commit if the generated SVG files need to be staged.
