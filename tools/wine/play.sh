#!/bin/bash
# play.sh SECONDS : launch DT.EXE, click START, screenshot every 3s
mkdir -p /root/shots; rm -f /root/shots/*
cd $WINEPREFIX/drive_c/DT
WINEDEBUG=err+all,fixme-all wine explorer /desktop=DT,640x480 DT.EXE > /root/dtp.log 2>&1 &
sleep 10; xdotool mousemove 444 375 click 1
for i in $(seq 1 $(($1/3))); do sleep 3; import -window root /root/shots/p$(printf %02d $i).png; done
xwininfo -root -tree | grep "\"" | head -6
wineserver -k
