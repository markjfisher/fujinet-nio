#!/usr/bin/env python3
"""Shared dependency, toolchain and build entry points (no hardware access)."""

import argparse
import hashlib
import os
from pathlib import Path
import platform
import shutil
import shlex
import re
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

sys.dont_write_bytecode = True
import bootstrap

ROOT = bootstrap.ROOT
TOOLCHAIN = "arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi"
TOOLCHAIN_URL = (
    "https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/"
    + TOOLCHAIN
    + ".tar.xz"
)
TOOLCHAIN_SHA256 = "62a63b981fe391a9cbad7ef51b17e49aeaa3e7b0d029b36ca1e9c3b2a9b78823"


def workspace():
    for parent in ROOT.parents:
        if (parent / "scripts/env.sh").is_file() and (
            parent / "repos/fujinet-nio"
        ).is_dir():
            return parent
    return None


def compiler_works(directory):
    if any(
        not os.access(directory / ("arm-none-eabi-" + tool), os.X_OK)
        for tool in ("gcc", "g++", "ar", "objcopy")
    ):
        return False
    try:
        result = subprocess.run(
            [str(directory / "arm-none-eabi-gcc"), "--version"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return result.returncode == 0
    except OSError:
        return False


def install_toolchain(cache):
    if not hasattr(tarfile, "data_filter"):
        raise ValueError(
            "Toolchain installation requires Python 3.12+ (or a Python security update providing tarfile.data_filter)"
        )
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise ValueError("Automatic toolchain installation supports Linux x86_64 only")
    destination = cache / TOOLCHAIN
    if destination.exists() or destination.is_symlink():
        raise ValueError(
            f"Existing unusable toolchain at {destination}; preserve/inspect it before retrying"
        )
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".arm-install-", dir=cache) as tmp:
        staging = Path(tmp)
        archive = staging / "toolchain.tar.xz"
        print(f"Downloading pinned toolchain: {TOOLCHAIN_URL}", flush=True)
        urllib.request.urlretrieve(TOOLCHAIN_URL, archive)
        digest = hashlib.sha256()
        with archive.open("rb") as data:
            for block in iter(lambda: data.read(1024 * 1024), b""):
                digest.update(block)
        checksum = digest.hexdigest()
        if checksum != TOOLCHAIN_SHA256:
            raise ValueError(
                "Toolchain SHA-256 checksum mismatch; archive was not extracted"
            )
        with tarfile.open(archive) as source:
            source.extractall(staging, filter="data")
        extracted = staging / TOOLCHAIN
        if not compiler_works(extracted / "bin"):
            raise ValueError("Downloaded toolchain compiler is unusable")
        extracted.rename(destination)
    return destination / "bin"


def configure_toolchain(install=False, required=True):
    explicit = os.environ.get("PICO_TOOLCHAIN_PATH")
    cache = (workspace() or ROOT) / "build/toolchains"
    if explicit:
        candidates = [Path(explicit).absolute(), Path(explicit).absolute() / "bin"]
        selected = next((p for p in candidates if compiler_works(p)), None)
        if selected is None:
            raise ValueError("Invalid explicit PICO_TOOLCHAIN_PATH: " + explicit)
    else:
        found = shutil.which("arm-none-eabi-gcc")
        candidates = ([Path(found).parent] if found else []) + [
            cache / TOOLCHAIN / "bin",
            Path.home() / ".local/toolchains/arm-none-eabi/bin",
        ]
        selected = next((p for p in candidates if compiler_works(p)), None)
        if selected is None and install:
            selected = install_toolchain(cache)
    if selected is None:
        if required:
            raise ValueError(
                f"Missing usable ARM compiler; run {ROOT / 'scripts/setup.sh'} --install-toolchain"
            )
        return None
    selected = selected.resolve()
    os.environ["PICO_TOOLCHAIN_PATH"] = str(selected)
    os.environ["PATH"] = str(selected) + os.pathsep + os.environ.get("PATH", "")
    return selected


def requirements(host_only=False, usb=True):
    def executable(variable, default):
        tokens = shlex.split(os.environ.get(variable, default))
        if not tokens:
            raise ValueError(f"{variable} must name a compiler executable")
        return tokens[0]

    tools = ["git", "cmake", "ctest", "ninja", executable("CC", "cc")]
    if not host_only:
        tools += [executable("CXX", "c++")]
        if usb:
            tools += ["pkg-config"]
    missing = [tool for tool in tools if not shutil.which(tool)]
    if missing:
        raise ValueError("Missing prerequisite tools: " + ", ".join(missing))
    version_text = subprocess.check_output(["cmake", "--version"], text=True)
    version = re.search(r"cmake version (\d+)\.(\d+)", version_text)
    if not version or tuple(map(int, version.groups())) < (3, 21):
        raise ValueError(
            "CMake >=3.21 is required; install a newer CMake and ensure it is first on PATH"
        )
    if not host_only and usb:
        if subprocess.run(["pkg-config", "--exists", "libusb-1.0"]).returncode:
            raise ValueError(
                "Missing libusb-1.0 development files required for the USB loader"
            )


def validate_compiler_cache(build_directory):
    """A compiler switch requires a fresh CMake build tree, not only a cache flag."""
    cache = Path(build_directory) / "CMakeCache.txt"
    if not cache.exists():
        return
    selected = Path(os.environ["PICO_TOOLCHAIN_PATH"])
    values = dict(
        re.findall(
            r"^(CMAKE_C(?:XX)?_COMPILER):[^=]+=(.*)$", cache.read_text(), re.MULTILINE
        )
    )
    for language, compiler in (("C", "gcc"), ("CXX", "g++")):
        cached = values.get(f"CMAKE_{language}_COMPILER")
        expected = (selected / ("arm-none-eabi-" + compiler)).resolve()
        if cached and Path(cached).resolve() != expected:
            raise ValueError(
                f"Cached {language} compiler {cached} differs from selected {expected}. "
                f"Move {build_directory} aside to preserve its contents, then rerun the build command."
            )


def run(argv):
    subprocess.run(list(map(str, argv)), cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    setup = commands.add_parser("setup", help="set up dependencies and validate tools")
    setup.add_argument("--host-only", action="store_true")
    setup.add_argument("--repair", action="store_true")
    setup.add_argument("--install-toolchain", action="store_true")
    commands.add_parser("test", help="build and test native Debug and Release")
    build = commands.add_parser("build", help="build firmware without loading hardware")
    build.add_argument(
        "preset", choices=["firmware", "stimulus-rp2040"], default="firmware", nargs="?"
    )
    commands.add_parser(
        "install-toolchain", help="install pinned compiler only if none is usable"
    )
    args = parser.parse_args()
    if args.command == "install-toolchain":
        print(configure_toolchain(install=True))
        return
    host_only = args.command == "test" or getattr(args, "host_only", False)
    if host_only and getattr(args, "install_toolchain", False):
        parser.error("--host-only cannot be combined with --install-toolchain")
    mode = "host" if host_only else "all" if args.command == "setup" else "firmware"
    repair = getattr(args, "repair", False)
    # Explicit source repair is useful even if a later compiler prerequisite fails.
    if repair:
        bootstrap.setup_sources(mode, repair=True)
    requirements(host_only, usb=args.command == "setup")
    if not host_only:
        configure_toolchain(getattr(args, "install_toolchain", False))
    if not repair:
        bootstrap.setup_sources(mode)
    if args.command == "test":
        for preset in ("host", "host-release"):
            run(["cmake", "--preset", preset])
            run(["cmake", "--build", "--preset", preset])
            run(["ctest", "--preset", preset])
    elif args.command == "build":
        validate_compiler_cache(ROOT / "build" / args.preset)
        run(
            [
                "cmake",
                "--preset",
                args.preset,
                "-DPICO_SDK_PATH=" + str(bootstrap.selected_sdk()),
                "-DPICO_TOOLCHAIN_PATH=" + os.environ["PICO_TOOLCHAIN_PATH"],
            ]
        )
        run(["cmake", "--build", "--preset", args.preset])


if __name__ == "__main__":
    try:
        main()
    except (
        ValueError,
        OSError,
        tarfile.TarError,
        subprocess.CalledProcessError,
    ) as error:
        print(f"setup: {error}", file=sys.stderr)
        sys.exit(1)
