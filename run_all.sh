#!/bin/bash

if [ $# -eq 0 ]
then
    echo "Usage: ./run_all.sh persistency_model[arp|epoch]"
    exit
fi

if ! [[ "$1" =~ ^(arp|epoch)$ ]]
then
    echo "Invalid persistency_model. Choose from [arp|epoch]"
    exit
fi

pModel=$1

tmux new -d -s hops_$pModel "./run.sh nstore $pModel; read" \;\
    new-window -d "./run.sh echo $pModel; read" \; next-window \;\
    new-window -d "./run.sh vacation $pModel; read" \; next-window \;\
    new-window -d "./run.sh memcached $pModel; read" \; next-window \;\
    new-window -d "./run.sh atlas_heap $pModel; read" \; next-window \;\
    new-window -d "./run.sh atlas_queue $pModel; read" \; next-window \;\
    new-window -d "./run.sh atlas_skiplist $pModel; read" \; next-window \;\
    new-window -d "./run.sh cceh $pModel; read" \; next-window \;\
    new-window -d "./run.sh fair $pModel; read" \; next-window \;\
    new-window -d "./run.sh dash_lh $pModel; read" \; next-window \;\
    new-window -d "./run.sh dash_ex $pModel; read" \; next-window \;\
    new-window -d "./run.sh recipe_art $pModel; read" \; next-window \;\
    new-window -d "./run.sh recipe_clht $pModel; read" \; next-window \;\
    new-window -d "./run.sh recipe_masstree $pModel; read" \; next-window \;\
    attach \;
