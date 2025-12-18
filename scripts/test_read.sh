#!/bin/bash

FN_PORT="/dev/ttyACM1"
if [ "$#" -ne 0 ]; then
  FN_PORT=$1
fi
KEY="qweuyroqiuwyeoruyqwoeuryoqiwueyroiquyweoriuyqwoieuryoqiuwyeroiuqyweoiruyqwoeiuryqoiwueyr"
VALUE="gkzjxhgcvkjhzgxckjvhgzkxjchgvkzjxhgcvkhzgxckvjhgzxkjchvgzkjxhcgvjzhxgcvkjhgzxckjhvgzkjxhcgvkjzhxgcvjhzgxckvjhgzxkcjvhgzx"

HANDLE=$(scripts/fujinet -p ${FN_PORT} net open "http://192.168.1.130:8080/get?${KEY}=${VALUE}&${VALUE}=${KEY}" | grep handle | cut -d= -f4)
echo "handle: $HANDLE"
sleep 0.2
scripts/fujinet -p ${FN_PORT} net info $HANDLE
scripts/fujinet -p ${FN_PORT} net read --max-bytes=2048 $HANDLE
scripts/fujinet -p ${FN_PORT} net close $HANDLE
