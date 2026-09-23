#!/usr/bin/env bash

read -p "FujiNet-NIO PTY [4]: " FUJINET_PTY
FUJINET_PTY=${FUJINET_PTY:-4}

read -p "Inject PTY [7]: " INJECT_PTY
INJECT_PTY=${INJECT_PTY:-7}

echo "Linking /dev/pts/${FUJINET_PTY} <-> /dev/pts/${INJECT_PTY}"

socat -d2 -ls -v \
  /dev/pts/${FUJINET_PTY},rawer,echo=0,icanon=0 \
  /dev/pts/${INJECT_PTY},rawer,echo=0,icanon=0