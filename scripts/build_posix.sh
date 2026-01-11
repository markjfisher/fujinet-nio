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
  echo "Usage: $(basename $0) [options] -- [additional args]"
  echo ""
  echo "fujinet-pc (cmake) options:"
  echo "   -c          # run clean before build"
  echo "   -p PROFILE  # choose configure profile from CMakePresets.json"
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

while getopts "chp:SV:" flag
do
  case "$flag" in
    c) DO_CLEAN=1 ;;
    p) CMAKE_PROFILE=${OPTARG} ;;
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

# Only (re)create/activate if we're not already in this venv
#if [ "$VIRTUAL_ENV" != "$VENV_ROOT" ]; then
#  if [ ! -f "$ACTIVATE" ]; then
#    echo "Creating venv at: $VENV_ROOT"
#    mkdir -p "$(dirname "$VENV_ROOT")"
#    "$PYTHON" -m venv "$VENV_ROOT" || {
#      echo "Failed to create venv at $VENV_ROOT"
#      exit 1
#    }
#  fi
#
#  . "$ACTIVATE" || {
#    echo "Failed to activate venv at $VENV_ROOT"
#    exit 1
#  }
#fi
#
#echo "Virtual env: $VIRTUAL_ENV"

##################################################################################
# cmake build
##################################################################################

#mkdir -p "$SCRIPT_DIR/build"
if [ $DO_CLEAN -eq 1 ] ; then
    echo "Removing old build artifacts"
    rm -rf $SCRIPT_DIR/build/${CMAKE_PROFILE}
fi

# TODO: enable this when we need python modules
# # python_modules.txt contains pairs of module name and installable package names, separated by pipe symbol
# MOD_LIST=$(sed '/^#/d' < "${SCRIPT_DIR}/python_modules.txt" | cut -d\| -f1 | tr '\n' ' ' | sed 's# *$##;s# \{1,\}# #g')
# echo "Checking python modules installed: $MOD_LIST"
# ${PYTHON} -c "import importlib.util, sys; sys.exit(0 if all(importlib.util.find_spec(mod.strip()) for mod in '''$MOD_LIST'''.split()) else 1)"
# if [ $? -eq 1 ] ; then
#   echo "At least one of the required python modules is missing"
#   bash ${SCRIPT_DIR}/install_python_modules.sh
# fi

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

## TODO: When we build a distribution, do something here.
# cmake --build --preset ${CMAKE_PROFILE}-build --target dist

ctest -V --progress --test-dir "build/${CMAKE_PROFILE}"
