# About this repository

This repository contains the gem5 simulator code for evaluating the design idea proposed in ASAP: A Speculative Approach to Persistence.
ASAP is implemented with gem5 v20.0.0.3.
There are 4 branches in this repository, each for a different model (baseline, ASAP, HOPS, ideal).

# Usage guide

This readme aims to help in the installation, compilation and execution of this simulator. The repository contains scripts to reproduce the results presented in the paper.

## Installing

1. Install gem5 dependencies.
    ```sh
    sudo apt install build-essentialgit m4 scons zlib1g zlib1g-dev libprotobuf-dev python python-dev protobuf-compiler libgoogle-perftools-dev libprotoc-dev libboost-all-dev pkg-config
    ```
    
2. Clone this repository.
    ```sh
    git clone https://github.com/multifacet/ASAP.git
    ```
   We use full system simulations to evaluate ASAP. gem5 full system simulations require a kernel image and disk images with the workloads. The kernel and disk images used for evaluating ASAP are available at:
    http://pages.cs.wisc.edu/~sujayyadalam/asap/
    
2. Download the kernel image into gem5 directory and the disk images to a directory called `disks`.
    ```sh
    cd ASAP
    wget http://pages.cs.wisc.edu/~sujayyadalam/asap/vmlinux_12
    mkdir disks; cd disks;
    wget -r -np -nd -A "*.img" http://pages.cs.wisc.edu/~sujayyadalam/asap/images
    cd ..
    ```
3. While running on Ubuntu 20.04, certain Python 2 modules needs to be installed. For this, we would require pip2.
   ```sh
   wget https://bootstrap.pypa.io/pip/2.7/get-pip.py
   python2 get-pip.py
   ```
   Install `six` module.
   ```sh
   python2 -m pip install six
   ```
    
## Building gem5

1. gem5 requires GCC version 7.0 or greater and scons >= 3.0. gem5 v20.0.0.3 (version of this repository) has some issues with Python 3. So use Python 2 to compile gem5.
    ```sh
    python2 $(which scons) build/X86/gem5.fast -j<threads>
    ```
    For more information about building gem5 refer to http://www.gem5.org/documentation/general_docs/building
    
2. If you wish to change the model, then remove the `build` folder before re-compiling.
    ```sh
    git checkout <branch>
    rm -r build
    python2 $(which scons) build/X86/gem5.fast -j<threads>
    ```
    
 ## Evaluating workloads
 
 The disk images mentioned above include all the workloads used for evaluating ASAP. If you wish to run these workloads, you could use the `run.sh` script.
 
    ./run.sh <workload>
    
 If you are evaluating ASAP or HOPS, an additional parameter for the persistency model [arp|epoch] needs to be passed.
 
    ./run.sh <workload> <persistency_model>
    
 If you wish to run all the workloads in parallel, you could use the `run_all.sh` script. Note that this would require atleast 16 cores and 150GB for memory.
 
    ./run_all.sh
    
 ## Reproducing results from the paper
 
 To reproduce the performance comparison results presented in the graph, you could use the `reproduce_results.py`. Invoke this script after you have completed all the experiments, i.e. all workloads with all models.
 
    ./reproduce_results.py
 
 # License
 
 This artifact uses the same license as gem5. See `LICENSE` for more information.
