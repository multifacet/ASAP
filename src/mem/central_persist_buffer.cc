#include "mem/central_persist_buffer.hh"

CentralPersistBuffer*
CentralPersistBufferParams::create()
{
    return new CentralPersistBuffer(this);
}

void
CentralPersistBuffer::checkAndMarkNVM(PacketPtr pkt) {
    // YSSU: Temp fix to make MC 3 and 4 as NVM
    for (int nvmPort=2+numThreads; nvmPort<=3+numThreads; nvmPort++) {
        AddrRangeList l = masterPorts[nvmPort]->getAddrRanges();
        for (auto it = l.begin(); it != l.end(); ++it) {
            if (it->contains(pkt->getAddr())) {
                pkt->req->setFlags(Request::NVM);
                break;
            }
        }
    }
}

bool
CentralPersistBuffer::recvTimingReq(PacketPtr pkt, PortID idx){
    bool status = false;

    checkAndMarkNVM(pkt);

    if (pkt->isWrite() && idx >= numThreads) { // Writes from Cache to Memory
        if (pkt->req->isNVM()) { // PM accesses, only DMA allowed to pass
            if (pkt->req->isDMA() &&
                bufferedAddr.find(alignAddr(pkt->getAddr())) !=
                bufferedAddr.end()) {
                assert(false && "DMA write conflicts with buffered Addr");
            }
            else if (pkt->req->isDMA()) {
                status = masterPorts[idx]->sendTimingReq(pkt);
            }
            else {
                status = masterPorts[idx]->sendTimingReq(pkt);
            }
        }
        else { //Volatile accesses pass through
            status = masterPorts[idx]->sendTimingReq(pkt);
        }
    }
    else if (pkt->isWrite() && idx < numThreads) {
        //Only catch PM writes from cores to caches
        // DFence
        if (pkt->req->isDFence()) {
            assert(0);
        }
        // Ofence
        else if (pkt->req->isOFence()) {
            assert(0);
        }
        // Acquire
        else if (pkt->req->isAcquire()) {
            assert(0);
        }
        // Release
        else if (pkt->req->isRelease()) {
            assert(0);
        }
        else if (pkt->req->isNVM()) { // PM accesses
            pmAccesses++;
            status = masterPorts[idx]->sendTimingReq(pkt);
        }
        else { //Volatile access pass through
            dramAccesses++;
            status = masterPorts[idx]->sendTimingReq(pkt);
        }
    }
    else if (pkt->isRead() && idx >= numThreads) { // Reads from caches to MCs
        // Avoid reads resulting from write misses from being caught here
        if (pkt->req->isWriteMiss()) {
            /* Read miss corresponding to L1 write miss */
            status = masterPorts[idx]->sendTimingReq(pkt);
        }
        /* An actual READ from the Caches to Memory */
        else  {
            status = masterPorts[idx]->sendTimingReq(pkt);
        }
    }
    /* Lost Ownership Request */
    else if (pkt->isOwnershipLost() && pkt->req->isNVM()) {
        /*
        LostOwnership Packet isn't really used because in our model, the caches
        don't have visibility Into TS, so we can only check TS after coherence,
        which is too late!  Thus we simply check for conflicts on the
        CPU-cache path
        */
        delete pkt;
        status = true;
    }
    else if (pkt->isOwnershipLost()) {
        // This for VM access doesn't make sense so just delete pkt..
        delete pkt;
        status = true;
    }
    else { /* Reads from CPU to cache */
        if (pkt->req->isNVM() && pkt->isRead())
            pmAccesses++;
        else if (!pkt->req->isNVM() && pkt->isRead())
            dramAccesses++;
        status = masterPorts[idx]->sendTimingReq(pkt);
    }
    return status;

}

bool
CentralPersistBuffer::recvTimingResp(PacketPtr pkt, PortID idx){
    bool status;

    status = slavePorts[idx]->sendTimingResp(pkt);
    return status;
}

void
CentralPersistBuffer::regStats()
{
    ClockedObject::regStats();
    using namespace Stats;

    missConflict
        .name(name() + ".missConflict")
        .desc("Number of conflicting LLC misses");

    interTEpochConflict
        .name(name() + ".interTEpochConflict")
        .desc("Number of conflicting accesses from other threads");

    entriesInserted
        .name(name() + ".entriesInserted")
        .desc("Number of entries inserted into all PBs");

    ofenceTotal
        .name(name() + ".ofenceTotal")
        .desc("Number of ofences seen across all PBs");

    dfenceTotal
        .name(name() + ".dfenceTotal")
        .desc("Number of dfences seen across all PBs");

    entriesFlushed
        .name(name() + ".entriesFlushed")
        .desc("Number of entries flushed from all PBs");

    pmAccesses
        .name(name() + ".pmAccesses")
        .desc("Number of accesses to PM");

    dramAccesses
        .name(name() + ".dramAccesses")
        .desc("Number of accesses to DRAM");

    bwSatCycles
        .name(name() + ".bwSatCycles")
        .desc("Cycles flushing stopped due to saturated BW");
}

