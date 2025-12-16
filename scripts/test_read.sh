#!/bin/bash

HANDLE=$(scripts/fujinet -p /dev/ttyACM1 net open "http://192.168.1.130:8080/get?foo=bar" | grep handle | cut -d= -f4)
echo "handle: $HANDLE"
sleep 0.2
scripts/fujinet -p /dev/ttyACM1 net info $HANDLE
scripts/fujinet -p /dev/ttyACM1 net read $HANDLE
scripts/fujinet -p /dev/ttyACM1 net close $HANDLE
