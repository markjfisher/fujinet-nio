#!/usr/bin/env python3

import os
import sys
import argparse


def build_sdkconfig_path(name: str) -> str:
    """Return the path to the sdkconfig defaults file for a given name."""
    return os.path.join("pio-build", "sdkconfig", f"sdkconfig-{name}.defaults")


def get_names_from_map(map_file: str, build_board: str):
    """
    Read the map file and return the list of sdkconfig names for the given build_board.

    Map file format:

        # comment
        cdc-fujibus-s3-wroom-1-n16r8=common,fs-littlefs,spiram-oct80,tinyusb
        sio-legacy-s3-wroom-1-n16r8=common,fs-littlefs,spiram-oct80,tinyusb
    """
    if not os.path.exists(map_file):
        print(f"Error: map file does not exist: {map_file}", file=sys.stderr)
        sys.exit(1)

    names = None

    with open(map_file, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            if "=" not in line:
                # Ignore malformed lines silently
                continue

            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip()

            if key == build_board:
                names = [n.strip() for n in value.split(",") if n.strip()]
                break

    if names is None:
        print(
            f"Error: build_board '{build_board}' not found in map file '{map_file}'.",
            file=sys.stderr,
        )
        sys.exit(1)

    if not names:
        print(
            f"Error: build_board '{build_board}' has no sdkconfig names in map file '{map_file}'.",
            file=sys.stderr,
        )
        sys.exit(1)

    return names


def concatenate_sdkconfig_files(names, output_file):
    """Concatenate sdkconfig-<name>.defaults files and sdkconfig.local.defaults into output_file."""
    input_files = []

    # Add all sdkconfig-<name>.defaults files
    for name in names:
        name = name.strip()
        if not name:
            continue
        path = build_sdkconfig_path(name)
        if not os.path.exists(path):
            print(f"Error: sdkconfig defaults file does not exist: {path}", file=sys.stderr)
            sys.exit(1)
        input_files.append(path)

    # Add sdkconfig.local.defaults at the end (must exist)
    if not os.path.exists("sdkconfig.local.defaults"):
        print("Error: sdkconfig.local.defaults file does not exist", file=sys.stderr)
        sys.exit(1)

    input_files.append("sdkconfig.local.defaults")

    if not input_files:
        print("Error: No input sdkconfig files to concatenate.", file=sys.stderr)
        sys.exit(1)

    try:
        with open(output_file, "w") as out_f:
            for path in input_files:
                with open(path, "r") as in_f:
                    out_f.write(in_f.read())
        print(f"Created '{output_file}' from:")
        for path in input_files:
            print(f"  {path}")
    except OSError as e:
        print(f"Error writing to output file '{output_file}': {e}", file=sys.stderr)
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="sdkconfig.defaults generator (from map file)")
    parser.add_argument(
        "-o",
        "--output-file",
        metavar="output_file",
        required=True,
        help="Output sdkconfig.defaults file to create",
    )
    parser.add_argument(
        "-m",
        "--map-file",
        metavar="map_file",
        required=True,
        help="Path to map file containing build_board=sdkconfig_name,... mappings",
    )
    parser.add_argument(
        "-b",
        "--build-board",
        metavar="build_board",
        required=True,
        help="Build board key to look up in the map file (e.g. 'sio-legacy-s3-wroom-1-n16r8')",
    )

    args = parser.parse_args()

    names = get_names_from_map(args.map_file, args.build_board)
    concatenate_sdkconfig_files(names, args.output_file)


if __name__ == "__main__":
    main()
