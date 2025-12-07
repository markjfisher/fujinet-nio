#!/usr/bin/env bash
#
# an interface to running fujinet-nio builds for pio targets

set -euo pipefail

# ASSUMPTION: this is one directory deeper than the repo root
SCRIPT_DIR=$(realpath $( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/..)
VIRTUAL_ENV=${VIRTUAL_ENV:-}
PIO_VENV_ROOT="${PLATFORMIO_CORE_DIR:-${HOME}/.platformio/penv}"

# Option defaults
RUN_BUILD=0
DO_CLEAN=0
SHOW_MONITOR=0
SHOW_BOARDS=0
TARGET_NAME=""
UPLOAD_IMAGE=0
UPLOAD_FS=0
ZIP_MODE=0
AUTOCLEAN=1
SETUP_NEW_BOARD=""
ANSWER_YES=0
CREATE_SDKCONFIG_DEFAULTS=""

# These are no longer specified as args, that was only for firmware build to ensure users didn't lose the hand crafted ini.
# This build will always generate the platformio.ini from combining local and global ini files
INI_FILE="${SCRIPT_DIR}/platformio.ini"
LOCAL_PIO_INI="${SCRIPT_DIR}/platformio.local.ini"
LOCAL_SKDCONFIG_DEFAULTS="${SCRIPT_DIR}/sdkconfig.local.defaults"
SDKCONFIG_MAP_FILE="${SCRIPT_DIR}/pio-build/sdkconfig/platform_sdkconfig_map.txt"

function display_board_names {
  while IFS= read -r piofile; do
    BOARD_NAME=$(echo $(basename $piofile) | sed 's#^platformio-##;s#.ini$##')
    echo "$BOARD_NAME"
  done < <(find "$SCRIPT_DIR/pio-build/ini/platforms" -name 'platformio-*.ini' -print | sort)
}

function show_help {
  echo "Usage: $(basename $0) [options] -- [additional args]"
  echo ""
  echo "fujinet-nio options:"
  echo "   -c       # run clean before build"
  echo "   -b       # run build"
  echo "   -u       # upload firmware"
  echo "   -f       # upload filesystem (webUI etc)"
  echo "   -m       # run monitor after build"
  echo "   -n       # do not autoclean"
  echo ""
  echo "fujinet-nio board setup options:"
  echo "   -s NAME  # Setup a new board from name, writes a new file 'platformio.local.ini', and 'sdkconfig.local.defaults' file"
  echo ""
  echo "other options:"
  echo "   -y       # answers any questions with Y automatically, for unattended builds"
  echo "   -h       # this help"
  echo "   -V       # Override default Python virtual environment location (e.g. \"-V ~/.platformio/penv\")"
  echo "            # Alternatively, this can be set with the shell env var VENV_ROOT"
  echo ""
  echo "Supported boards:"
  echo ""
  display_board_names
  exit 1
}

if [ $# -eq 0 ] ; then
  show_help
fi

while getopts "bcfhmns:SuyV:z" flag
do
  case "$flag" in
    b) RUN_BUILD=1 ;;
    c) DO_CLEAN=1 ;;
    f) UPLOAD_FS=1 ;;
    m) SHOW_MONITOR=1 ;;
    n) AUTOCLEAN=0 ;;
    s) SETUP_NEW_BOARD=${OPTARG} ;;
    S) SHOW_BOARDS=1 ;;
    u) UPLOAD_IMAGE=1 ;;
    y) ANSWER_YES=1  ;;
    V) VENV_ROOT=${OPTARG} ;;
    z) ZIP_MODE=1 ;;
    h) show_help ;;
    *) show_help ;;
  esac
done
shift $((OPTIND - 1))

if [ $SHOW_BOARDS -eq 1 ] ; then
  display_board_names
  exit 1
fi

##################################################################################
# Python check and venv setup
##################################################################################
PYTHON=$(command -v python3 || command -v python) || {
  echo "Python is not installed"; exit 1;
}

"$PYTHON" -c 'import sys, venv, ensurepip; sys.exit(0 if sys.version_info[0] == 3 else 1)' \
  || { echo "Need Python 3 with venv and ensurepip available"; exit 1; }

# Default venv location if VENV_ROOT not set
VENV_ROOT="${VENV_ROOT:-${PLATFORMIO_CORE_DIR:-${HOME}/.platformio/penv}}"
ACTIVATE="${VENV_ROOT}/bin/activate"

# Only (re)create/activate if we're not already in this venv
if [ "$VIRTUAL_ENV" != "$VENV_ROOT" ]; then
  if [ ! -f "$ACTIVATE" ]; then
    echo "Creating venv at: $VENV_ROOT"
    mkdir -p "$(dirname "$VENV_ROOT")"
    "$PYTHON" -m venv "$VENV_ROOT" || {
      echo "Failed to create venv at $VENV_ROOT"
      exit 1
    }
  fi

  . "$ACTIVATE" || {
    echo "Failed to activate venv at $VENV_ROOT"
    exit 1
  }
fi

echo "Virtual env: $VIRTUAL_ENV"

##################################################################################
# PIO
##################################################################################

# If pio is the one installed by the system it runs the system
# python instead of the penv python, blocking pip from installing
# packages
PIO=$(command -v pio)
if [ "${PIO}" != "${VENV_ROOT}/bin/pio" ] ; then
    pip install platformio || exit 1
fi
command -v pio

ZIP_INI_ARGS=""
if [ ${ZIP_MODE} -eq 1 ] ; then
  ZIP_INI_ARGS="-f pio-build/ini/platformio.zip-options.ini"
fi

if [ -z "$SETUP_NEW_BOARD" ] ; then
  # Did not specify -s flag, so do not overwrite local changes with new board
  # but do re-generate the INI/SDKDEFAULTS files, this ensures upstream changes are pulled into
  # existing builds (e.g. upgrading platformio version)

  # Check the local ini file has been previously generated as we need to read which board the user is building
  if [ ! -f "$LOCAL_PIO_INI" ] ; then
    echo "ERROR: local platformio ini file not found. Please run build.sh with -s flag first."
    echo ""
    show_help
  fi

  ${PYTHON} ${SCRIPT_DIR}/pio-build/scripts/create-platformio-ini.py -o $INI_FILE -l $LOCAL_PIO_INI ${ZIP_INI_ARGS}
  # creates if it doesn't exist, but leaves existing file unchanged
  touch ${LOCAL_SKDCONFIG_DEFAULTS}
else
  # this will create a clean platformio INI file, but honours the command line args
  if [ -e ${LOCAL_PIO_INI} -a $ANSWER_YES -eq 0 ] ; then
    echo "WARNING! This will potentially overwrite any local changes in $LOCAL_PIO_INI"
    echo -n "Do you want to proceed? (y|N) "
    read answer
    answer=$(echo $answer | tr '[:upper:]' '[:lower:]')
    if [ "$answer" != "y" ]; then
      echo "Aborting"
      exit 1
    fi
  fi
  ${PYTHON} ${SCRIPT_DIR}/pio-build/scripts/create-platformio-ini.py -n $SETUP_NEW_BOARD -o $INI_FILE -l $LOCAL_PIO_INI ${ZIP_INI_ARGS}
  rm -f ${LOCAL_SKDCONFIG_DEFAULTS}
  touch ${LOCAL_SKDCONFIG_DEFAULTS}
fi

# Create the sdkconfig.defaults file from the map file
${PYTHON} ${SCRIPT_DIR}/pio-build/scripts/create-sdkconfig.py -o sdkconfig.defaults -m ${SDKCONFIG_MAP_FILE} -b ${SETUP_NEW_BOARD}

##############################################################
# Now call pio to run the various build steps
##############################################################

TARGET_ARG=""
if [ -n "${TARGET_NAME}" ] ; then
  TARGET_ARG="-t ${TARGET_NAME}"
fi

if [ ${DO_CLEAN} -eq 1 ] ; then
  pio run -t clean
fi

AUTOCLEAN_ARG=""
if [ ${AUTOCLEAN} -eq 0 ] ; then
  AUTOCLEAN_ARG="--disable-auto-clean"
fi

if [ ${RUN_BUILD} -eq 1 ] ; then
  pio run $TARGET_ARG $AUTOCLEAN_ARG 2>&1
fi

if [ ${UPLOAD_FS} -eq 1 ] ; then
  pio run -t uploadfs 2>&1
fi

if [ ${UPLOAD_IMAGE} -eq 1 ] ; then
  pio run -t upload 2>&1
fi

if [ ${SHOW_MONITOR} -eq 1 ] ; then
  pio device monitor 2>&1
fi
