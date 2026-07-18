# KiChad agent guide

## Mission

KiChad is a Codex-integrated downstream of KiCad.  Preserve KiCad file-format compatibility,
licensing, undo/redo behavior, and cross-platform architecture.  Keep AI provider code behind a
small interface so local, hosted, and future providers can be swapped without editor rewrites.

## Repository contract

- `origin` is `10-X-eng/KiChad` on GitHub; `upstream` is canonical KiCad on GitLab.
- Base active development on the exact stable tag named by `.kichad-base-version`; do not track
  upstream development branches or release candidates.  Do not modify upstream release branches/tags.
- Generated output belongs under `build/`; do not commit binaries, generated lexers, or local user
  configuration.
- Never commit model-provider credentials, tokens, prompts containing private board data, or user
  project content.

## Build and validation

On Ubuntu 24.04 or KDE neon, run `./tools/bootstrap-kichad-ubuntu.sh` once.  The normal loop is:

```sh
./tools/build-kichad.sh
```

Pass one or more target names to the script for a narrower build.  The equivalent manual preset
commands are documented in `KICHAD.md`.

Use `build/install/bin/kichad-cli version` as the headless smoke check and
`./tools/run-kichad.sh` for a GUI smoke check.  Run the narrowest relevant CTest/QA target for edited
code; use `ctest --preset kichad-release` only when broad coverage is warranted.  Report warnings
separately from failures.

## Integration constraints

- Use Codex app-server dynamic tools hosted in-process; do not add MCP or a separate tool server.
- Use the official KiCad 10 IPC API for live PCB operations.  Schematic and library edits must use
  the lossless s-expression layer and validate through KiCad after every mutation.
- Keep network and inference work off the wxWidgets UI thread.  Cancellation and timeout paths are
  required for any model request.
- Represent editor changes as existing tool actions/commands so they participate in undo, project
  dirty-state tracking, autosave, and tests.
- Treat schematics, boards, credentials, and prompts as sensitive.  Require an explicit user action
  before sending project data to a remote provider, and make the payload inspectable.
- Match surrounding KiCad C++ style and use the repository's formatting hooks for touched code.

## Upstream compatibility

Before changing shared core behavior, inspect the same area on the pinned upstream stable line.
Keep KiChad-only
branding and integration code localized; avoid broad renames that turn routine upstream syncs into
repository-wide conflicts.
