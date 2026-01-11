#!/usr/bin/env bash
#
# an interface to running fujinet-nio builds for cmake targets
set -euo pipefail

# ASSUMPTION: this is one directory deeper than the repo root
SCRIPT_DIR=$(realpath $( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/..)
VIRTUAL_ENV=${VIRTUAL_ENV:-}

# Option defaults
DO_CLEAN=0
CMAKE_PROFILE=""
SHOW_PROFILES=0

function show_profiles {
  if [ ! -x "$(command -v jq)" ]; then
    echo "jq is required to show available profiles"
    exit 1
  fi
  echo "Available profiles:"
  jq -r '
    .configurePresets as $p |
    ($p | map(.name | length) | max) as $w |
    $p[] |
    (.name + (" " * ($w - (.name | length))) + " - " + .displayName)
  ' "${SCRIPT_DIR}/CMakePresets.json"

}

function show_help {
  echo "Usage: $(basename $0) [options] PROFILE_NAME -- [additional args]"
  echo ""
  echo "   -c          # run clean before build"
  echo "   -S          # show available profiles from CMakePresets.json"
  echo ""
  echo "other options:"
  echo "   -h          # this help"
  echo "   -V          # Override default Python virtual environment location (e.g. \"-V ~/.platformio/penv\")"
  echo "               # Alternatively, this can be set with the shell env var VENV_ROOT"
  echo ""
  show_profiles
  exit 1
}

if [ $# -eq 0 ] ; then
  show_help
fi

while getopts "chpSV:" flag
do
  case "$flag" in
    c) DO_CLEAN=1 ;;
    p) ;; # this is just a "posix" flag, the profile is now an argument
    S) SHOW_PROFILES=1 ;;
    V) VENV_ROOT=${OPTARG} ;;
    h) show_help ;;
    *) show_help ;;
  esac
done
shift $((OPTIND - 1))

if [ $SHOW_PROFILES -eq 1 ] ; then
  show_profiles
  exit 0
fi

if [ $# -eq 0 ]; then
  echo "No profile selected. Please specify one on the command line"
  show_help
fi

CMAKE_PROFILE="$1"
shift  # Remove profile from $@ so it's not passed to cmake

##################################################################################
# Python check and venv setup
##################################################################################
# Default venv location if VENV_ROOT not set
VENV_ROOT="${VENV_ROOT:-"${SCRIPT_DIR}/build/.venv"}"
ACTIVATE="${VENV_ROOT}/bin/activate"

mkdir -p "$SCRIPT_DIR/build"

# Python uv needs to be installed with pipx if not already present
if ! uv --version &> /dev/null ; then
  echo "Python uv is required but not found.  Installing... "
  pipx install uv 
  export PATH="$HOME/.local/bin:$PATH"
fi

uv venv --clear "$VENV_ROOT"
source "$ACTIVATE"
uv sync --active

PYTHON=$(command -v python3 || command -v python) || {
  echo "Python is not installed"; exit 1;
}

"$PYTHON" -c 'import sys, venv, ensurepip; sys.exit(0 if sys.version_info[0] == 3 else 1)' \
  || { echo "Need Python 3 with venv and ensurepip available"; exit 1; }


##################################################################################
# cmake build
##################################################################################

if [ $DO_CLEAN -eq 1 ] ; then
    echo "Removing old build artifacts"
    rm -rf $SCRIPT_DIR/build/${CMAKE_PROFILE}
fi

##################################################################
# CMake Profiles
#
# We use the convention that the profile given is the configure
# profile in the file CMakePresets.json. The build and test profiles
# are derived from the configure profile by appending "-build" and
# "-test" respectively.
##################################################################

cmake --preset ${CMAKE_PROFILE} "$@"
cmake --build --preset ${CMAKE_PROFILE}-build

## TODO: Review distribution builds. Currently the distfiles folder is copied into the build as part of standard posix build, see FN_BUILD_POSIX_APP in CMakeLists_posix.cmake
## which copies the run-fujinet-nio shell script, facilitating easy reboot of posix instance via exit codes.
# cmake --build --preset ${CMAKE_PROFILE}-build --target dist

ctest -V --progress --test-dir "build/${CMAKE_PROFILE}"
