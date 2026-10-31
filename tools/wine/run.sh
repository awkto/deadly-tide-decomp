#!/bin/bash
# $1 = exe (relative to C:\DT), $2 = seconds, shots every 6s into /root/shots
mkdir -p /root/shots; rm -f /root/shots/*
cd $WINEPREFIX/drive_c/DT
WINEDEBUG=err+all,fixme-all wine explorer /desktop=DT,640x480 $1 > /root/dtp.log 2>&1 &
for i in $(seq 1 $(($2/6))); do sleep 6; import -window root /root/shots/s$(printf %02d $i).png; done
xwininfo -root -tree | grep "\"" | head -5
wineserver -k
