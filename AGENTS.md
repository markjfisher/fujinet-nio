# Project Context

This repository contains the firmware for the Fujinet network adapter.

Key documents to read are:
- [Context Bootstrap](docs/context_bootstrap.md) for the basic information and links to other docs in the project

## Architecture rule: no platform `#ifdef`s in shared code

Core and `src/lib/` code carries no platform `#ifdef`s and includes no
`platform/<name>/` headers (see `docs/architecture.md`, "1. No `#ifdef`
spaghetti", and `docs/context_bootstrap.md`, "What “no ifdefs” means in
practice"). Platform behaviour lives under `src/platform/<name>/` and is
selected at registration time; when shared code needs a platform effect, inject
it (for example a callback passed to the factory), as `FileActivityHook` does.

## Building and Testing

To compile the project with its tests run:

```shell
# Posix
./build.sh -cp fujibus-pty-debug

# esp32
./build.sh -b
```

## Adding New Source Files

When new files are added to the project you can run the [update sources script](scripts/update_cmake_sources.py) to automatically update all cmake files.
Review its diff before keeping it: it can rewrite hand-maintained parts of
`CMakeLists_posix.cmake` (build options and the per-profile `build_profile/`
selection). If it does, revert and add the new file to the source lists by hand.

## Changing a device protocol or adding a setting

The firmware is only half of every feature. When you add or change a device
command, a wire field, or a configurable value, finish the other halves in the
same piece of work:

1. **Python client: always.** `py/fujinet_tools` is a first-class client, not
   a test convenience. Every command change needs its request builder and
   response parser, a CLI subcommand where a person would use it
   (`./scripts/fujinet ...`), unit tests in `py/tests`, and the command name
   in `analyze_capture.py`. Run `./scripts/run-python-tests`.
2. **Diagnostics console: ask.** If the change adds state worth inspecting or
   a setting someone may tune, ask the user whether to expose it through a
   diagnostics provider (`src/lib/diagnostic_*_provider.cpp`, documented in
   [diagnostics](docs/diagnostics.md)). Lean towards yes for configurable
   values: a value in `fujinet.yaml` should normally be viewable, settable and
   savable from the console, as `uart.*` and `link.*` are. Add the console
   output to the provider's tests.
3. **Protocol docs and compatibility.** Update the device's
   `docs/*_protocol.md`. New or extended commands follow
   [extending commands](docs/protocol_reference.md#extending-commands), so
   newer clients work with older firmware and the reverse.
4. **Other clients.** C clients use `fujinet-nio-lib` in the workspace; a
   command they need gets its library call there too (`make check`).

## Architecture diagrams

The code-derived architecture/UML diagrams (`gen_*`) are built by a GitHub
workflow on pushes to master in fujinet-nio/fujinet-nio (forks skip it) and
published to GitHub Pages; they are not committed. Never generate or commit
them, and don't mention them in PR descriptions.
