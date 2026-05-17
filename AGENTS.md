# Project Context

This repository contains the firmware for the Fujinet network adapter.

Key documents to read are:
- [Context Bootstrap](docs/context_bootstrap.md) for the basic information and links to other docs in the project

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
