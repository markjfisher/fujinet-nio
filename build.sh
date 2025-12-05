#!/usr/bin/env bash

# an interface to running fujinet-nio builds
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
PIO_VENV_ROOT="${PLATFORMIO_CORE_DIR:-${HOME}/.platformio/penv}"
PC_VENV_ROOT="${SCRIPT_DIR}/build/.venv"

RUN_BUILD=0
ENV_NAME=""
DO_CLEAN=0
SHOW_MONITOR=0
SHOW_BOARDS=0
TARGET_NAME=""
PC_TARGET=""
DEBUG_PC_BUILD=0
UPLOAD_IMAGE=0
UPLOAD_FS=0
DEV_MODE=0
ZIP_MODE=0
AUTOCLEAN=1
SETUP_NEW_BOARD=""
ANSWER_YES=0
CMAKE_GENERATOR=""
INI_FILE="${SCRIPT_DIR}/platformio.ini"

# Function to check if the specified Python version is 3
check_python_version() {
  local python_bin=$1

  if ! command -v "${python_bin}" &> /dev/null; then
    return 1
  fi

  # Extract the major version number
  local major_version="$(${python_bin} --version 2>&1 | cut -d' ' -f2 | cut -d'.' -f1)"

  # Verify if it's Python 3
  if [ "${major_version}" -eq 3 ]; then
    return 0
  else
    return 1
  fi
}

function display_board_names {
  while IFS= read -r piofile; do
    BOARD_NAME=$(echo $(basename $piofile) | sed 's#^platformio-##;s#.ini$##')
    echo "$BOARD_NAME"
  done < <(find "$SCRIPT_DIR/build-platforms" -name 'platformio-*.ini' -print | sort)
}

function show_help {
  echo "Usage: $(basename $0) [options] -- [additional args]"
  echo ""
  echo "fujinet-firmware (pio) options:"
  echo "   -c       # run clean before build"
  echo "   -b       # run build"
  echo "   -u       # upload firmware"
  echo "   -f       # upload filesystem (webUI etc)"
  echo "   -m       # run monitor after build"
  echo "   -n       # do not autoclean"
  echo ""
  echo "fujinet-pc (cmake) options:"
  echo "   -c       # run clean before build"
  echo "   -p TGT   # perform PC build instead of ESP, for given target (e.g. APPLE|ATARI)"
  echo "   -g       # enable debug in generated fujinet-pc exe"
  echo "   -G GEN   # Use GEN as the Generator for cmake (e.g. -G \"Unix Makefiles\" )"
  echo ""
  echo "other options:"
  echo "   -y       # answers any questions with Y automatically, for unattended builds"
  echo "   -h       # this help"
  echo "   -V       # Override default Python virtual environment location (e.g. \"-V ~/.platformio/penv\")"
  echo "            # Alternatively, this can be set with the shell env var VENV_ROOT"
  echo ""
  echo "Additional Args can be accepted to pass values onto sub processes where supported."
  echo "  e.g. ./build.sh -p APPLE -- -DFOO=BAR"
  echo ""
  echo "Simple pio builds:"
  echo "    ./build.sh -cb        # for CLEAN + BUILD of current target in platformio.ini"
  echo "    ./build.sh -m         # View FujiNet Monitor"
  echo "    ./build.sh -cbum      # Clean/Build/Upload to FN/Monitor"
  echo "    ./build.sh -f         # Upload filesystem"
  echo ""
  echo "Supported boards:"
  echo ""
  display_board_names
  exit 1
}

if [ $# -eq 0 ] ; then
  show_help
fi

while getopts "bcfgG:hmnp:uyV:" flag
do
  case "$flag" in
    b) RUN_BUILD=1 ;;
    c) DO_CLEAN=1 ;;
    f) UPLOAD_FS=1 ;;
    g) DEBUG_PC_BUILD=1 ;;
    m) SHOW_MONITOR=1 ;;
    n) AUTOCLEAN=0 ;;
    p) PC_TARGET=${OPTARG} ;;
    S) SHOW_BOARDS=1 ;;
    u) UPLOAD_IMAGE=1 ;;
    G) CMAKE_GENERATOR=${OPTARG} ;;
    y) ANSWER_YES=1  ;;
    V) VENV_ROOT=${OPTARG} ;;
    h) show_help ;;
    *) show_help ;;
  esac
done
shift $((OPTIND - 1))

# Requirements:
#   - python3
#   - python3 can create venv - PlatformIO also needs this to install penv
#   - if doing ESP32 build:
#     - PlatformIO
#   - not ESP32 build:
#     - cmake

# Make sure we have python3 and it has the ability to create venvs
PYTHON=python
if ! check_python_version "${PYTHON}" ; then
    PYTHON=python3
    if ! check_python_version "${PYTHON}" ; then
        echo "Python 3 is not installed"
        exit 1
    fi
fi

if ! ${PYTHON} -c "import venv, ensurepip" 2>/dev/null ; then
    echo "Error: Python venv module is not installed."
    exit 1
fi

if [ -z "${VENV_ROOT}" ] ; then
    if [ -n "${PC_TARGET}" ] ; then
        VENV_ROOT="${PC_VENV_ROOT}"
    else
        VENV_ROOT="${PIO_VENV_ROOT}"
    fi
fi

ACTIVATE="${VENV_ROOT}/bin/activate"
if [ -z "${PC_TARGET}" ] ; then
    # Doing a PlatformIO build, locate PlatformIO. It may or may not
    # already be in the users' path.
    if [ -f "${ACTIVATE}" ] ; then
        # Activate now in case pio isn't already in PATH
        source "${ACTIVATE}"
    fi
    PIO=$(command -v pio)
    if [ -z "${PIO}" ] ; then
        echo Please install platformio
        exit 1
    fi
fi

same_dir() {
    [ -d "$1" ] && [ -d "$2" ] || return 1
    stat1=$(stat -c "%d:%i" "$1")
    stat2=$(stat -c "%d:%i" "$2")
    [ "$stat1" = "$stat2" ]
}

if [[ "$VIRTUAL_ENV" != "$VENV_ROOT" ]] ; then
    if [ ! -f "${ACTIVATE}" ] ; then
        echo Creating venv at "${VENV_ROOT}"
        mkdir -p $(dirname "${VENV_ROOT}")
        ${PYTHON} -m venv "${VENV_ROOT}" || exit 1
    fi
    if [ -f "${ACTIVATE}" ] ; then
        source "${ACTIVATE}"
    elif [ -f "${ALT_ACTIVATE}" ] ; then
        source "${ALT_ACTIVATE}"
        echo "-------------------"
        cat "${ALT_ACTIVATE}"
        echo "-------------------"
    fi
    VENV_ACTUAL="$(normalize_path "$VIRTUAL_ENV")"
    if ! same_dir "${VENV_ACTUAL}" "${VENV_ROOT}" ; then
        echo Unable to activate penv/venv
        echo "ACTIVATE = ${ACTIVATE}"
        echo "ALT_ACTIVATE = ${ALT_ACTIVATE}"
        echo "VIRTAUL_ENV = ${VIRTUAL_ENV}"
        echo "VENV_ACTUAL = ${VENV_ACTUAL}"
        echo "VENV_ROOT = ${VENV_ROOT}"
        ls -Fla "$(dirname ${ACTIVATE})" || true
        ls -Fla "$(dirname ${ALT_ACTIVATE})" || true
        exit 1
    fi
fi

# If pio is the one installed by the system it runs the system
# python instead of the penv python, blocking pip from installing
# packages
if [ -z "${PC_BUILD}" ] ; then
    PIO=$(command -v pio)
    if [ "${PIO}" != "${VENV_ROOT}/bin/pio" ] ; then
        pip install platformio || exit 1
    fi
fi

echo Virtual env: "${VIRTUAL_ENV}"
echo venv root: "${VENV_ROOT}"
echo Activate: "${ACTIVATE}"
echo PATH: "${PATH}"
command -v pio

if [ $SHOW_BOARDS -eq 1 ] ; then
  display_board_names
  exit 1
fi

if [ $BUILD_ALL -eq 1 ] ; then
  # BUILD ALL platforms and exit
  chmod 755 $SCRIPT_DIR/build-platforms/build-all.sh
  $SCRIPT_DIR/build-platforms/build-all.sh
  exit $?
fi

##############################################################
# PC BUILD using cmake
if [ ! -z "$PC_TARGET" ] ; then
  echo "PC Build Mode"
  # lets build_webui.py know we are using the generated INI file, this variable name is the one PIO uses when it calls subprocesses, so we use same name.
  export PROJECT_CONFIG=$INI_FILE
  GEN_CMD=""
  if [ -n "$CMAKE_GENERATOR" ] ; then
    GEN_CMD="-G $CMAKE_GENERATOR"
  fi

  mkdir -p "$SCRIPT_DIR/build"
  LAST_TARGET_FILE="$SCRIPT_DIR/build/last-target"
  LAST_TARGET=""
  if [ -f "${LAST_TARGET_FILE}" ]; then
    LAST_TARGET=$(cat ${LAST_TARGET_FILE})
  fi
  if [[ (-n ${LAST_TARGET}) && ("${LAST_TARGET}" != "$PC_TARGET") ]] ; then
    DO_CLEAN=1
  fi
  echo -n "$PC_TARGET" > ${LAST_TARGET_FILE}

  if [ $DO_CLEAN -eq 1 ] ; then
    echo "Removing old build artifacts"
    rm -rf $SCRIPT_DIR/build/*
    rm -f $SCRIPT_DIR/build/.ninja* 2>/dev/null
  fi

  cd $SCRIPT_DIR/build
  # Write out the compile commands for clangd etc to use
  if [ -z "$GEN_CMD" ]; then
    cmake .. -DFUJINET_TARGET=$PC_TARGET "$@"
  else
    cmake "$GEN_CMD" .. -DFUJINET_TARGET=$PC_TARGET "$@"
  fi
  if [ $? -ne 0 ]; then
    echo "cmake failed writing compile commands. Exiting"
    exit 1
  fi
  # Run the specific build
  BUILD_TYPE="Release"
  if [ $DEBUG_PC_BUILD -eq 1 ] ; then
    BUILD_TYPE="Debug"
  fi

  echo "Building for $BUILD_TYPE"
  if [ -z "$GEN_CMD" ]; then
    cmake .. -DFUJINET_TARGET=$PC_TARGET -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@"
  else
    cmake "$GEN_CMD" .. -DFUJINET_TARGET=$PC_TARGET -DCMAKE_BUILD_TYPE=$BUILD_TYPE "$@" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  fi
  if [ $? -ne 0 ] ; then
    echo "Error running initial cmake. Aborting"
    exit 1
  fi

  # # python_modules.txt contains pairs of module name and installable package names, separated by pipe symbol
  # MOD_LIST=$(sed '/^#/d' < "${SCRIPT_DIR}/python_modules.txt" | cut -d\| -f1 | tr '\n' ' ' | sed 's# *$##;s# \{1,\}# #g')
  # echo "Checking python modules installed: $MOD_LIST"
  # ${PYTHON} -c "import importlib.util, sys; sys.exit(0 if all(importlib.util.find_spec(mod.strip()) for mod in '''$MOD_LIST'''.split()) else 1)"
  # if [ $? -eq 1 ] ; then
  #   echo "At least one of the required python modules is missing"
  #   bash ${SCRIPT_DIR}/install_python_modules.sh
  # fi

  cmake --build .
  if [ $? -ne 0 ] ; then
    echo "Error running actual cmake build. Aborting"
    exit 1
  fi

  # write it into the dist dir
  cmake --build . --target dist
  if [ $? -ne 0 ] ; then
    echo "Error running cmake distribution. Aborting"
    exit 1
  fi

  # run unit tests
  ctest -V --progress
    if [ $? -ne 0 ] ; then
    echo "Error running unit tests. Aborting"
    exit 1
  fi

  echo "Built PC version in build/dist folder"
  exit 0
fi

# if [ -z "$SETUP_NEW_BOARD" ] ; then
#   # Did not specify -s flag, so do not overwrite local changes with new board
#   # but do re-generate the INI file, this ensures upstream changes are pulled into
#   # existing builds (e.g. upgrading platformio version)

#   # Check the local ini file has been previously generated as we need to read which board the user is building
#   if [ ! -f "$LOCAL_INI_VALUES_FILE" ] ; then
#     echo "ERROR: local platformio ini file not found."
#     echo "Please see documentation in build-sh.md, and re-run build as follows:"
#     echo "   ./build.sh -s BUILD_BOARD"
#     echo "BUILD_BOARD values include:"
#     for f in $(ls -1 build-platforms/platformio-*.ini); do
#       BASE_NAME=$(basename $f)
#       BOARD_NAME=$(echo ${BASE_NAME//.ini} | cut -d\- -f2-)
#       echo " - $BOARD_NAME"
#     done
#     echo "This is only required to be done once."
#     exit 1
#   fi

#   if [ ${ZIP_MODE} -eq 1 ] ; then
#     ${PYTHON} create-platformio-ini.py -o $INI_FILE -l $LOCAL_INI_VALUES_FILE -f platformio-ini-files/platformio.zip-options.ini
#   else
#     ${PYTHON} create-platformio-ini.py -o $INI_FILE -l $LOCAL_INI_VALUES_FILE
#   fi
#   create_result=$?
# else
#   # this will create a clean platformio INI file, but honours the command line args
#   if [ -e ${LOCAL_INI_VALUES_FILE} -a $ANSWER_YES -eq 0 ] ; then
#     echo "WARNING! This will potentially overwrite any local changes in $LOCAL_INI_VALUES_FILE"
#     echo -n "Do you want to proceed? (y|N) "
#     read answer
#     answer=$(echo $answer | tr '[:upper:]' '[:lower:]')
#     if [ "$answer" != "y" ]; then
#       echo "Aborting"
#       exit 1
#     fi
#   fi
#   if [ ${ZIP_MODE} -eq 1 ] ; then
#     ${PYTHON} create-platformio-ini.py -n $SETUP_NEW_BOARD -o $INI_FILE -l $LOCAL_INI_VALUES_FILE -f platformio-ini-files/platformio.zip-options.ini
#   else
#     ${PYTHON} create-platformio-ini.py -n $SETUP_NEW_BOARD -o $INI_FILE -l $LOCAL_INI_VALUES_FILE
#   fi

#   create_result=$?
# fi
# if [ $create_result -ne 0 ] ; then
#   echo "Could not run build due to previous errors. Aborting"
#   exit $create_result
# fi

##############################################################
# NORMAL BUILD MODES USING pio

TARGET_ARG=""
if [ -n "${TARGET_NAME}" ] ; then
  TARGET_ARG="-t ${TARGET_NAME}"
fi

if [ ${DO_CLEAN} -eq 1 ] ; then
  pio run -t clean ${ENV_ARG}
fi

AUTOCLEAN_ARG=""
if [ ${AUTOCLEAN} -eq 0 ] ; then
  AUTOCLEAN_ARG="--disable-auto-clean"
fi

# any stage that fails from this point should stop the script immediately, as they are designed to run
# on from each other sequentially as long as the previous passed.
set -e

if [ ${RUN_BUILD} -eq 1 ] ; then
  pio run ${DEV_MODE_ARG} $ENV_ARG $TARGET_ARG $AUTOCLEAN_ARG 2>&1
fi

if [ ${UPLOAD_FS} -eq 1 ] ; then
  pio run ${DEV_MODE_ARG} -t uploadfs 2>&1
fi

if [ ${UPLOAD_IMAGE} -eq 1 ] ; then
  pio run ${DEV_MODE_ARG} -t upload 2>&1
fi

if [ ${SHOW_MONITOR} -eq 1 ] ; then
  pio device monitor 2>&1
fi
