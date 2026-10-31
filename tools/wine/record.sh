#!/bin/bash
# record.sh SECONDS OUT.mp4 : launch DT.EXE, START, skip intro, wiggle+fire, record 320x240 game area
cd $WINEPREFIX/drive_c/DT
WINEDEBUG=-all wine explorer /desktop=DT,640x480 DT.EXE > /root/dtp.log 2>&1 &
sleep 10; xdotool mousemove 444 375 click 1; sleep 8
ffmpeg -v error -y -f x11grab -framerate 15 -video_size 320x240 -i $DISPLAY+0,0 -t $1 -c:v libx264 -crf 16 -pix_fmt yuv420p $2 &
FF=$!
end=$((SECONDS+$1))
while [ $SECONDS -lt $end ]; do
  xdotool key space; sleep 1
  for k in 1 2 3 4 5 6; do xdotool mousemove $((40+RANDOM%240)) $((40+RANDOM%160)); xdotool mousedown 1; sleep 0.4; xdotool mouseup 1; sleep 0.3; done
done
wait $FF; wineserver -k
