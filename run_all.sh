#!/bin/bash

tmux new -d -s baseline './run.sh nstore; read' \;\
    new-window -d './run.sh echo; read' \; next-window \;\
    new-window -d './run.sh vacation; read' \; next-window \;\
    new-window -d './run.sh memcached; read' \; next-window \;\
    new-window -d './run.sh atlas_heap; read' \; next-window \;\
    new-window -d './run.sh atlas_queue; read' \; next-window \;\
    new-window -d './run.sh atlas_skiplist; read' \; next-window \;\
    new-window -d './run.sh cceh; read' \; next-window \;\
    new-window -d './run.sh fair; read' \; next-window \;\
    new-window -d './run.sh dash_lh; read' \; next-window \;\
    new-window -d './run.sh dash_ex; read' \; next-window \;\
    new-window -d './run.sh recipe_art; read' \; next-window \;\
    new-window -d './run.sh recipe_clht; read' \; next-window \;\
    new-window -d './run.sh recipe_masstree; read' \; next-window \;\
    attach \;
