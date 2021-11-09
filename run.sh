#!/bin/bash

export M5_PATH=/mydata

if [ $# -eq 0 ]
then
    echo "Usage: run.sh workload persistency_model[arp|epoch]"
fi

if ! [[ "$1" =~ ^(nstore|echo|vacation|memcached|atlas_heap|atlas_queue|atlas_skiplist|cceh|fast_fair|dash_lh|dash_ex|recipe_art|recipe_clht|recipe_masstree)$ ]]
then
    echo "Invalid workload. Select from nstore|echo|vacation|memcached|atlas_heap|atlas_queue|atlas_skiplist|cceh|fast_fair|dash_lh|dash_ex|recipe_art|recipe_clht|recipe_masstree"
    exit
fi

if ! [[ "$1" =~ ^(dash_lh|dash_ex|recipe_art|recipe_clht|recipe_masstree)$ ]]
then
    image=part1.img;
else
    image=part2.img;
fi

if [ "$1" == "memcached" ]
then
    cores=9
else
    cores=5
fi

if [ "$2" == "arp" ]
then
    pModel="arp"
elif [ "$2" == "epoch" ]
then
    pModel="epoch"
else
    echo $2
    echo "Invalid persistency model"
    exit
fi

sudo build/X86/gem5.fast -d results/hops_$2/$1 configs/example/fs.py --pmem --pmem-pwq --persist-buffers --pmem-wr-latency=60ns --mem-size=48GB --cpu-type=X86KvmCPU --kernel=/mydata/vmlinux_12 --disk-image=$image --ruby --asap -n $cores --script=scripts/$1.rcS --persistency-model=$pModel
