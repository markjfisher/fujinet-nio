#!/usr/bin/env bash
set -euo pipefail

# 1. Ensure build dir + compile_commands.json exist
mkdir -p build
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j

# 2. Run clang-uml using your config
clang-uml -c clang-uml.yml

# 3. Render all generated .puml to SVG
#    Note: output_directory in clang-uml.yml is docs/uml,
#          so we render those into docs/images.
mkdir -p docs/images
plantuml -tsvg docs/uml/*.puml -o ../images
