#!/usr/bin/python3

import os
import sys
import matplotlib.pyplot as plt
import numpy as np

RESULTS_DIR = "results"
MODELS = ['baseline', 'hops_epoch', 'hops_arp', 'asap_epoch', 'asap_arp', 'ideal']
WORKLOADS = ['nstore', 'echo', 'vacation', 'memcached', 'atlas_heap', 'atlas_queue',
            'atlas_skiplist', 'cceh', 'fast_fair', 'dash_lh', 'dash_ex', 'recipe_art',
            'recipe_clht', 'recipe_masstree']

sim_times = dict()
models = os.listdir(RESULTS_DIR)

if len(models) != len(MODELS):
    print("Please complete experiments with all models before running this script!")
    print("Models found:" + str(models))
    exit(1)

for model in models:
    sim_times[model] = dict()
    workloads = os.listdir(RESULTS_DIR+'/'+model)
    for workload in workloads:
        f = open(RESULTS_DIR+'/'+model+'/'+workload+'/'+'stats.txt', 'r')
        count = 0 # Interested in the 2nd sim_seconds line
        for line in f.readlines():
            if 'sim_seconds' in line:
                if count == 1:
                    sim_times[model][workload] = float(line.split()[1])
                count += 1
                if count == 2:
                    break
        f.close()

speedup = dict()

for m in MODELS:
    speedup[m] = []
    for w in WORKLOADS:
        if w in sim_times[m]:
            speedup[m].append(sim_times['baseline'][w]/sim_times[m][w])
        else:
            speedup[m].append(0)

print ("SPEEDUPS:")
print ("-----------------------------------------------------------------------------")
for m in speedup:
    print(str(m) + ": " + str(speedup[m]) + "\n")
print ("-----------------------------------------------------------------------------")

fig, ax = plt.subplots()

ind = np.arange(len(WORKLOADS))
width = 0.12

plt.grid(axis='y', which='major', linestyle='--')
plt.ylabel("Speedup", size=14)
ax.bar(ind, speedup['baseline'], width, color='#85929e', label='Baseline', edgecolor='black')
ax.bar(ind + width, speedup['hops_epoch'], width, color='#ec7063',\
        label='HOPS_EP', edgecolor='black', hatch='/')
ax.bar(ind + 2*width, speedup['asap_epoch'], width, color='#7fb3d5',\
        label='ASAP_EP', edgecolor='black', hatch='/')
ax.bar(ind + 3*width, speedup['hops_arp'], width, color='#ec7063',\
        label='HOPS_RP', edgecolor='black', hatch='.')
ax.bar(ind + 4*width, speedup['asap_arp'], width, color='#7fb3d5',\
        label='ASAP_RP', edgecolor='black', hatch='.')
ax.bar(ind + 5*width, speedup['ideal'], width, color='#eaf146',\
        label='EADR/BBB', edgecolor='black', hatch='')
ax.set(xticks=ind + 2*width + width/2, xticklabels=WORKLOADS, xlim=[2*width - 1, len(WORKLOADS)])
ax.xaxis.label.set_size(12)
ax.set_yticks(ax.get_yticks()[::2])
plt.xticks(rotation=45)

ax.legend(loc='upper right', ncol=3, prop={'size':8})
plt.show()

