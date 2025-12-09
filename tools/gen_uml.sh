#!/usr/bin/env bash
set -euo pipefail
set -x

echo "Generating UML diagrams..."
date

./build.sh -cp fujibus-pty-debug
clang-uml -c clang-uml.yml

mkdir -p docs/images
plantuml -v --duration -tsvg docs/uml/*.puml -o ../images

echo "Done generating UML diagrams."
date
