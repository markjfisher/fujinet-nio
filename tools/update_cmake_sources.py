#!/usr/bin/env python3
import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]  # assuming tools/ under repo root
SRC_ROOT = REPO_ROOT / "src"

POSIX_CMAKE = REPO_ROOT / "fujinet_posix.cmake"
ESP32_CMAKE = SRC_ROOT / "CMakeLists.txt"

START_MARKER = "# __TARGET_SOURCES_START__"
END_MARKER   = "# __TARGET_SOURCES_END__"


def collect_cpp_files():
    """Return all .cpp paths under src/ as POSIX-style strings."""
    files = []
    for root, _, filenames in os.walk(SRC_ROOT):
        for name in filenames:
            if not name.endswith(".cpp"):
                continue
            full = Path(root) / name
            rel = full.relative_to(REPO_ROOT)  # e.g. src/lib/foo.cpp
            files.append(rel.as_posix())
    return sorted(files)


def generate_posix_block(all_cpp):
    """
    Generate the CMake snippet for fujinet-nio (POSIX library target).
    Rules:
      - include src/lib/*.cpp, src/platform/posix/*.cpp
      - exclude anything under platform/esp32/
      - exclude src/app/main_posix.cpp
      - exclude src/app/main_esp32.cpp
    """
    filtered = []
    for path in all_cpp:
        if path == "src/app/main_posix.cpp":
            continue
        if path == "src/app/main_esp32.cpp":
            continue
        if "/platform/esp32/" in path:
            continue
        # we *do* want platform/posix/*.cpp, lib/*.cpp, etc.
        filtered.append(path)

    lines = ["target_sources(fujinet-nio", "    PRIVATE"]
    for path in filtered:
        lines.append(f"        {path}")
    lines.append(")")
    return "\n".join(lines) + "\n"


def generate_esp32_block(all_cpp):
    """
    Generate the idf_component_register() block for ESP-IDF (src/CMakeLists.txt).
    Rules:
      - paths relative to src/ (no leading 'src/')
      - include app/main_esp32.cpp
      - exclude app/main_posix.cpp
      - exclude platform/posix/*
    """
    filtered = []
    for path in all_cpp:
        if path == "src/app/main_posix.cpp":
            continue
        if "/platform/posix/" in path:
            continue
        # keep everything else (libs + platform/esp32 + app/main_esp32)
        if path.startswith("src/"):
            rel = path[len("src/"):]  # strip src/ prefix
        else:
            rel = path
        filtered.append(rel)

    lines = ["idf_component_register(", "    SRCS"]
    for path in filtered:
        lines.append(f"        {path}")
    return "\n".join(lines) + "\n"


def replace_block_in_file(path: Path, new_block: str):
    text = path.read_text(encoding="utf-8")

    try:
        start_idx = text.index(START_MARKER)
        end_idx = text.index(END_MARKER)
    except ValueError:
        raise SystemExit(f"Markers not found in {path}")

    before = text[:start_idx + len(START_MARKER)]
    after = text[end_idx:]

    # We ensure there's exactly one newline after the start marker,
    # then our block, then the end marker on its own line.
    replaced = before + "\n" + new_block + END_MARKER + after.split(END_MARKER, 1)[1]
    path.write_text(replaced, encoding="utf-8")
    print(f"Updated {path}")


def main():
    all_cpp = collect_cpp_files()

    posix_block = generate_posix_block(all_cpp)
    esp32_block = generate_esp32_block(all_cpp)

    # For POSIX, we want the full block between markers to be:
    #
    # # __TARGET_SOURCES_START__
    # target_sources(...)
    # # __TARGET_SOURCES_END__
    #
    replace_block_in_file(POSIX_CMAKE, posix_block)

    # For ESP32 (src/CMakeLists.txt) similarly:
    replace_block_in_file(ESP32_CMAKE, esp32_block)


if __name__ == "__main__":
    main()
