#!/bin/bash
# usage: /data/dt_tools/runall.sh -- dump every .L96 under /data/cd/cd*, 4 at a time, biggest first -> /data/dump/cdN/NAME
cd /data; mkdir -p dump/logs; rm -f dump/logs/ALLDONE
ls -S cd/cd*/*.L96 | xargs -P4 -I{} sh -c 'd=$(basename $(dirname {})); b=$(basename {} .L96); nice -n 10 python3 dt_tools/dump_assets.py {} dump/$d > dump/logs/${d}_${b}.txt 2>&1'
echo ALLDONE > dump/logs/ALLDONE
