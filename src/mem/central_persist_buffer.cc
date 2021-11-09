#include "mem/central_persist_buffer.hh"

#define max_(a,b) a>b?a:b

CentralPersistBuffer*
CentralPersistBufferParams::create()
{
    return new CentralPersistBuffer(this);
}

void CentralPersistBuffer::processRegFlushEvent(){
    for (int i = 0; i < numThreads; ++i) {
        if (!perThreadPBs[i]->flushEvent.scheduled()) {
            schedule(perThreadPBs[i]->flushEvent, nextCycle());
        }
    }
    schedule(regFlushEvent, clockEdge(Cycles(flushInterval)));
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

    if (!flushingStarted) {
        schedule(regFlushEvent, clockEdge(Cycles(flushInterval)));
        flushingStarted = true;
    }

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
                status = true;

                Addr addr = alignAddr(pkt->getAddr());
                std::map<Addr,PortID>::iterator it = simpleDir.find(addr);
                if (it != simpleDir.end())
                    simpleDir.erase(it);
            }
        }
        else { //Volatile accesses pass through
            status = masterPorts[idx]->sendTimingReq(pkt);
        }
    }
    else if (pkt->isWrite() && idx < numThreads) {
        //Only catch PM writes from cores to caches
        int numEntries = perThreadPBs[idx]->getSize();
        // DFence
        if (pkt->req->isDFence()) {
            if (pModel.compare("epoch") == 0)
                useARP = false; // Start using epoch persistency

            dfenceTotal++;
            DPRINTF(PersistBuffer, "serviceDFence: NOW! PB%d[%d/%d] \n",
                    idx, numEntries, pbCapacity);
            if (perThreadPBs[idx]->getSize() == 0){
                perThreadPBs[idx]->serviceOFence();
                if (pkt->needsResponse()) {
                    pkt->makeResponse();
                    slavePorts[idx]->schedTimingResp(pkt, curTick());
                }
            }
            else {
                perThreadPBs[idx]->serviceDFence();
                perThreadPBs[idx]->dfencePkts.push_back(pkt);
                assert(perThreadPBs[idx]->dfencePkts.size()==1);
            }
            status = true;
        }
        // Ofence
        else if (pkt->req->isOFence()) {
            ofenceTotal++;
            perThreadPBs[idx]->serviceOFence();
            //DPRINTF(PersistBuffer, "serviceOFence: NOW! PB%d[%d/%d] \n",
            //        idx, numEntries, pbCapacity);
            if (pkt->needsResponse()) {
                pkt->makeResponse();
                slavePorts[idx]->schedTimingResp(pkt, curTick());
            }
            status = true;
        }
        // Acquire
        else if (pkt->req->isAcquire()) {
            if (useARP) {
                Addr lockAddr = alignAddr(pkt->getAddr());
                std::map<Addr, depPair>::iterator itr=lock_map.find(lockAddr);
                DPRINTF(PersistBuffer, "Acquire at core %d, Addr=0x%lx\n", idx, lockAddr);

                // Cross thread dependency exists
                if (itr != lock_map.end() && itr->second.first != idx) {
                    int srcCoreID = (itr->second).first;
                    timestamp srcEpoch = perThreadPBs[srcCoreID]->getCurrentTS() - 1;
                    DPRINTF(PersistBuffer, "Cross thread dep: Source:%d, %d \
                            Destination: %d, %d\n", srcCoreID, srcEpoch, idx,
                            perThreadPBs[idx]->getCurrentTS());

                    perThreadPBs[idx]->serviceOFence();
                    perThreadPBs[idx]->addCrossThreadDep(srcCoreID, srcEpoch);
                    interTEpochConflict++;
                }
            }
            status = true;

            if (pkt->needsResponse()) {
                pkt->makeResponse();
                slavePorts[idx]->schedTimingResp(pkt, curTick());
            }
        }
        // Release
        else if (pkt->req->isRelease()) {
            if (useARP) {
                Addr lockAddr = alignAddr(pkt->getAddr());
                DPRINTF(PersistBuffer, "Release at core %d, Addr=0x%lx\n", idx, lockAddr);

                // Update the lock map with this thread ID
                std::map<Addr, depPair>::iterator itr;
                itr = lock_map.find(lockAddr);
                if (itr == lock_map.end())
                    lock_map.insert(std::pair<Addr, depPair>(lockAddr,
                                depPair(idx, perThreadPBs[idx]->currVecTS[idx])));
                else
                    itr->second = depPair{idx,
                                     perThreadPBs[idx]->currVecTS[idx]};

                // Create a new epoch
                perThreadPBs[idx]->serviceOFence();
            }
            // Send response back
            if (pkt->needsResponse()) {
                pkt->makeResponse();
                slavePorts[idx]->schedTimingResp(pkt, curTick());
            }

            status = true;
        }
        else if (pkt->req->isNVM()) { // PM accesses
            if (system->isMemAddr(pkt->getAddr())){
                if (!useARP)
                    perThreadPBs[idx]->checkCrossDependency(pkt);
                status = perThreadPBs[idx]->tryCoalescePBEntry(pkt);
                if (status) {
                    status = masterPorts[idx]->sendTimingReq(pkt);
                    if (status) {
                        status = perThreadPBs[idx]->coalescePBEntry(pkt);
                        if (!status)
                            assert(0); // coalescing failed?
                    }
                }
                else {
                    /* Superseded fix = handled in timingCPU */
                    /* We need to have space for 2 entries to handle case
                       of unaligned accesses spanning two cachelines
                       These are sent as split packets, and sending retry
                       to just one is not handled. */
                    if (perThreadPBs[idx]->getSize() >= pbCapacity) {
                        DPRINTF(PersistBufferDebug, "PB full. Not accepting "
                                " entries! PB%d[%d/%d] \n", idx, numEntries,
                                pbCapacity);
                        status = false;
                        if (!perThreadPBs[idx]->isStalled()) {
                            perThreadPBs[idx]->stallPB();
                        }
                    }
                    else {
                        pmAccesses++;
                        PacketPtr tmp = new Packet(pkt, false, true);
                        tmp->setData(pkt->getConstPtr<uint8_t>());
                        status = masterPorts[idx]->sendTimingReq(pkt);
                        if (status) {
                            perThreadPBs[idx]->addPBEntry(tmp);
                        }
                        delete tmp;
                    }
                    if (numEntries >= flushThreshold) {
                        if (!perThreadPBs[idx]->flushEvent.scheduled())
                            schedule(perThreadPBs[idx]->flushEvent,
                                     nextCycle());
                    }
                    if (!perThreadPBs[idx]->pollGlobalTSEvent.scheduled())
                        schedule(perThreadPBs[idx]->pollGlobalTSEvent,
                                nextCycle());
                }
            }
            else {
                DPRINTF(PersistBufferDebug, "Request address %#x assumed"
                        " to be a pio address\n", pkt->getAddr());
                status = masterPorts[idx]->sendTimingReq(pkt);
            }
        }
        else { //Volatile access pass through
            dramAccesses++;
            DPRINTF(PersistBufferDebug, "recvTimingReq: VM access addr:0x%x"
                    " value:%x\n", pkt->getAddr(), *(pkt->getPtr<uint8_t>()));
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
            if (pkt->req->isNVM() &&
                bufferedAddr.find(alignAddr(pkt->getAddr()))
                != bufferedAddr.end()) {
                //TODO FIXME, what here? ADD DELAY?? or buffer it
                status = masterPorts[idx]->sendTimingReq(pkt);
                missConflict++;
            }
            else{ // PM access or LLC miss not in the PB, business as usual
                status = masterPorts[idx]->sendTimingReq(pkt);
            }
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
        if (pkt->req->isNVM() && pkt->isRead()) {
            pmAccesses++;
        }
        else if (!pkt->req->isNVM() && pkt->isRead())
            dramAccesses++;
        status = masterPorts[idx]->sendTimingReq(pkt);
    }
    DPRINTF(PersistBufferDebug, "status=%d\n", status);
    return status;

}

bool
CentralPersistBuffer::recvTimingResp(PacketPtr pkt, PortID idx){
    bool status;

    /* Sink PM memory write responses here itself */
    if (pkt->req->isNVM() && pkt->isWrite() && idx >= numThreads &&
        !pkt->req->isPMFence()) {
        SenderState *s = dynamic_cast<SenderState *>(pkt->senderState);
        perThreadPBs[s->id]->flushAck(pkt);
        pkt->req.reset();
        delete pkt->popSenderState();
        delete pkt;
        status = true;
    }
    else if (pkt->isWrite() &&
            idx >= numThreads && pkt->req->isDMA()) {
        DPRINTF(PersistBufferDebug, "DMA Write to address range 0x%x:%x\n",
                pkt->getAddr(), pkt->getAddr()+pkt->getSize());
        status = slavePorts[idx]->sendTimingResp(pkt);
    }
    else {
        DPRINTF(PersistBufferDebug, "schedTimingResp id:%d Addr:0x%x"
                " isPMFence:%d\n", idx, pkt->getAddr(), pkt->req->isPMFence());
        status = slavePorts[idx]->sendTimingResp(pkt);
    }
    DPRINTF(PersistBufferDebug, "status=%d\n", status);
    return status;
}

void
CentralPersistBuffer::PersistBuffer::checkCrossDependency(PacketPtr pkt)
{
    Addr addr = pb.alignAddr(pkt->getAddr());
    std::map<Addr,PortID>::iterator it = pb.simpleDir.find(addr);
    // If directory contains addr, update cross-thread dep
    if (it != pb.simpleDir.end()) {
        PortID srcCore = it->second;
        if (srcCore != id) {
            timestamp srcEpoch = pb.perThreadPBs[srcCore]->getCurrentTS();
            pb.perThreadPBs[srcCore]->serviceOFence();
            DPRINTF(PersistBuffer, "Cross thread dep: Source:%d, %d \
                                Destination: %d\n", srcCore, srcEpoch, id);
            serviceOFence();
            addCrossThreadDep(srcCore, srcEpoch);
            pb.interTEpochConflict++;
        }
        it->second = id; // Update the directory
    }
    else {
        pb.simpleDir.insert(std::pair<Addr,PortID>(addr,id));
    }
}

bool
CentralPersistBuffer::PersistBuffer::tryCoalescePBEntry(PacketPtr pkt)
{

    bool coalesced = false;
    pbOccupancy.sample(PBEntries.size());

    //Coalesce stores if possible
    // if not present, no point looking further
    if (pb.bufferedAddr.find(pb.alignAddr(pkt->getAddr()))
            != pb.bufferedAddr.end()) {
        for (auto entry : PBEntries) {
            if (entry->getAddr() == pb.alignAddr(pkt->getAddr())
                            && !entry->getIsFlushing()) {
                if (compareVecTS(entry->getVecTS(), currVecTS)) {
                    coalesced = true;
                    break;
                }
            }
        }
    }
    return coalesced;
}

bool
CentralPersistBuffer::PersistBuffer::coalescePBEntry(PacketPtr pkt)
{
    bool coalesced;
    for (auto entry : PBEntries) {
        if (entry->getAddr() == pb.alignAddr(pkt->getAddr())
                    && !entry->getIsFlushing()) {
            if (compareVecTS(entry->getVecTS(), currVecTS)){
                DPRINTF(PersistBufferDebug, "PB%d ::Coalescing request"
                        " to addr 0x%x, req size:%d\n",
                        id, entry->getAddr(), pkt->getSize());
                DPRINTF(PersistBufferDebug, "PB%d::Old Write Mask%s\n",
                        id, entry->printMask());
                pkt->writeDataToBlock(entry->getDataPtr(), BLK_SIZE);
                entry->setMask(pkt->getOffset(BLK_SIZE),
                                pkt->getSize());
                DPRINTF(PersistBufferDebug, "PB%d::New Write Mask%s\n",
                        id, entry->printMask());
                numCoalesced++;
                epochEntries++;
                coalesced = true;
                break;
            }
        }
    }
    return coalesced;
}

void
CentralPersistBuffer::PersistBuffer::addPBEntry(PacketPtr pkt)
{
    PBEntry *newEntry = new PBEntry(pkt, currVecTS, id, pb.numThreads);
    pb.bufferedAddr.insert(pb.alignAddr(pkt->getAddr()));
    PBEntries.push_back(newEntry);

    epochEntries++;
    pb.entriesInserted++;
    unflushedEntries++;
    DPRINTF(PersistBuffer, "addPBEntry: Addr: 0x%lx\n", pkt->getAddr());
}

void
CentralPersistBuffer::PersistBuffer::flushPB(){
    int mc = -1;
    PBEntry *flushEntry = getOldestUnflushed();
    if (flushEntry == NULL) {
        return;
    }
    /* Check we are in the same epoch,
        else wait for previous epoch to flush completely */
    if (PBEntries.front()->getTS() != flushEntry->getTS()) {
        noflushCyclesIntra++;
        return;
    }

    for (int i=pb.numThreads; i<pb.numThreads+pb.numMCs; i++) {
        AddrRangeList l = pb.masterPorts[i]->getAddrRanges();
        for (auto it = l.begin(); it != l.end(); ++it) {
            if (it->contains(flushEntry->getAddr())) {
                mc = i;
                break;
            }
        }
        if (mc == i)
            break;
    }
    assert(mc != -1); // mc cannot be -1

   if (pb.memSaturated[mc-pb.numThreads])
        return;

    RequestPtr req = std::make_shared<Request>(flushEntry->getAddr(),
                flushEntry->getSize(), 0, pb.masterId);
    req->setFlags(Request::NVM);

    PacketPtr pkt = Packet::createWrite(req);
    uint8_t *newData = new uint8_t[BLK_SIZE];
    pkt->dataDynamic(newData);
    memcpy(newData, flushEntry->getDataPtr(), BLK_SIZE);
    pkt->setMask(flushEntry->getMask());

    SenderState *s = new SenderState(id);
    pkt->pushSenderState(s);

    bool flushed = pb.masterPorts[mc]->sendTimingReq(pkt);
    if (flushed) {
        DPRINTF(PersistBufferDebug, "flushPB: Flushing addr"
                " 0x%x via memPort%d\n", pkt->getAddr(), mc);
        flushEntry->setIsFlushing();
        unflushedEntries--;
    } else {
        pb.memSaturated[mc-pb.numThreads] = true;
        if (pb.bwSatStart[mc-pb.numThreads] == 0)
            pb.bwSatStart[mc-pb.numThreads] = curTick();
        DPRINTF(PersistBuffer, "BW saturated at MC %d\n", mc);
        pkt->req.reset();
        delete pkt->popSenderState();
        delete pkt;
    }
}

void
CentralPersistBuffer::PersistBuffer::flushAck(PacketPtr pkt){
    DPRINTF(PersistBufferDebug, "Flush ack for PB%d, retryWrReq=%d, size=%d,"
           " threshold=%d\n", id, retryWrReq, PBEntries.size(),
           0.9 * pb.pbCapacity);
    /* Doesn't matter which entry is flushed ..*/
    PBEntry *flushEntry = PBEntries.front();
    timestamp oldTS = flushEntry->getTS();
    assert(flushEntry->getIsFlushing());
    delete flushEntry;
    PBEntries.pop_front();
    pb.entriesFlushed++;

    // Hack for removing one copy of addr from buffer
    Addr a = pb.alignAddr(pkt->getAddr());
    auto location = pb.bufferedAddr.find(a);
    if (location != pb.bufferedAddr.end()) {
        pb.bufferedAddr.erase(location);
    }
    //Shouldn't be here, if an addr is buffered,
    // it should be present
    else assert(false);

    if (PBEntries.size()==0){
        if (dFenceInProg)
            respondToDFence();

        // New epoch started but writes not received yet
        // All epochs till current epoch complete
        if (oldTS < currVecTS[id]) {
            pb.globalTS[id] = currVecTS[id] - 1;
        }
    }
    // Youngest entry belongs to next epoch => current epoch complete
    else if (oldTS != PBEntries.front()->getTS()) {
        pb.globalTS[id] = PBEntries.front()->getTS() - 1;
    }

    if (retryWrReq) {
        DPRINTF(PersistBufferDebug, "flushAck:Unblocked! Sending retry request"
                " to core%d\n", id);
        retryWrReq = false;
        assert(stallStart > 0);
        stallCycles += (curTick() - stallStart);
        stallStart = 0;
        pb.slavePorts[id]->sendRetryReq();
    }
}

void CentralPersistBuffer::PersistBuffer::processFlushEvent(){
    if (unflushedEntries > 0) { /* PB has buffered entries waiting to be flushed */
        timestamp* vecTS = getOldestUnflushed()->getVecTS();
        /* Safe to flush, as vecTS not ahead of global TS */
        if (flushOkay(vecTS)) {
            flushPB();
        }
        else { /* Stuck behind other cores, so we initiate flushes for them */
            noflushCyclesInter++;
            /* Flushing stuck behind other threads so flush them instead ..*/
            /*for (int i=0; i<pb.numThreads; i++) {
                if (vecTS[i]>readGlobalTS[i] && i!=id) {
                    if (!pb.perThreadPBs[i]->flushEvent.scheduled()) {
                        schedule(pb.perThreadPBs[i]->flushEvent,
                                 pb.nextCycle());
                    }
                }
            }*/
        }
        //REMEMBER TO CORRECT BELOW STUFF
        // When flushing becomes async, as the size wont change
        if (dFenceInProg) { /* service dFence till buffer is emptied */
            if (unflushedEntries > 0){
                /* Keep flushing to satisfy dFence */
                if (!flushEvent.scheduled())
                    schedule(flushEvent, pb.nextCycle());
            }
        }
        else if (unflushedEntries >= pb.flushThreshold)  {
            /* if PB is still over-capacity, keep flushing */
            DPRINTF(PersistBufferDebug,
                    "PB%d[%d/%d] Threshold STILL exceeded!"
                    " Flushing in next Cycle\n",
                    id, getSize(), pb.pbCapacity);
            if (!flushEvent.scheduled())
                schedule(flushEvent, pb.nextCycle());
        }
    }
}

void CentralPersistBuffer::PersistBuffer::pollingGlobalTS() {
    for (int i = 0; i < pb.numThreads; ++i) {
        readGlobalTS[i] = pb.globalTS[i];
    }
    schedule(pollGlobalTSEvent, curTick() + pb.pollLatency);
}

void
CentralPersistBuffer::regStats()
{
    ClockedObject::regStats();
    using namespace Stats;

    for (auto p : perThreadPBs) {
        p->regStats();
    }

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
        .desc("Number of cycles flushing stopped due to saturated BW");
}

void
CentralPersistBuffer::PersistBuffer::regStats()
{
    using namespace Stats;

    epochSize
         .init(pb.pbCapacity+1)
         .name(name() + ".epochSize")
         .desc("Size of buffered epochs")
         .flags(nonan);

    pbOccupancy
         .init(pb.pbCapacity+1)
         .name(name() + ".pbOccupancy")
         .desc("Occupancy of perThread PBs")
         .flags(nonan);

    wawHits
         .init(pb.pbCapacity)
         .name(name() + ".wawHits")
         .desc("WAW reuse in an epoch")
         .flags(nozero);

    stallCycles
        .name(name() + ".cyclesStalled")
        .desc("Number of cycles stalled due to full PB");

    dfenceCycles
        .name(name() + ".dfenceStalled")
        .desc("Number of cycles stalled due to dfence");

    noflushCyclesIntra
        .name(name() + ".cyclesBlockedIntra")
        .desc("Number of cycles per PB stalled due to flushing block");

    noflushCyclesInter
        .name(name() + ".cyclesBlockedInter")
        .desc("Number of cycles per PB stalled due to flushing block");
}
