#!/usr/bin/env bash
set -euo pipefail
set -x

echo "Generating UML diagrams..."
date

BUILD_TARGET=fujibus-pty-debug

./build.sh -cp ${BUILD_TARGET}
clang-uml -c clang-uml.yml -d build/${BUILD_TARGET}

mkdir -p docs/images
plantuml -v --duration -tsvg docs/uml/*.puml -o ../images

echo "Done generating UML diagrams."
date
