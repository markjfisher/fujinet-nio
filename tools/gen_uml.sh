#!/usr/bin/env bash
set -euo pipefail
set -x

echo "Generating UML diagrams..."
date

BUILD_TARGET=fujibus-pty-debug

./build.sh -cp ${BUILD_TARGET}

mkdir -p docs/images
mkdir -p docs/uml

# remove old files in case their UML is removed.
rm -f docs/images/gen_*.svg
rm -f docs/uml/gen_*.puml

# generate new versions
clang-uml -c clang-uml.yml -d build/${BUILD_TARGET}
plantuml -v --duration -tsvg docs/uml/*.puml -o ../images

echo "Done generating UML diagrams."
date
