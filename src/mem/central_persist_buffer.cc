#include "mem/central_persist_buffer.hh"

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
                std::map<Addr, PortID>::iterator it = simpleDir.find(addr);
                simpleDir.erase(it);
                assert(it != simpleDir.end());
            }
        }
        else { //Volatile accesses pass through
            status = masterPorts[idx]->sendTimingReq(pkt);
        }
    }
    else if (pkt->isWrite() && idx < numThreads) {
        //Only catch PM writes from cores to caches

        // Dfence
        if (pkt->req->isDFence()) {
            if (pModel.compare("epoch") == 0)
                useARP = false; // Start using epoch persistency

            dfenceTotal++;
            if (perThreadETs[idx]->serviceDFence(pkt)) {
                if (pkt->needsResponse()) {
                    pkt->makeResponse();
                    slavePorts[idx]->schedTimingResp(pkt, curTick());
                }
            } else {
                if (!perThreadPBs[idx]->flushEvent.scheduled())
                    schedule(perThreadPBs[idx]->flushEvent, nextCycle());
            }
            status = true;
        }
        // Ofence
        else if (pkt->req->isOFence()) {
            if (perThreadETs[idx]->getSize() >= etCapacity) {
                perThreadETs[idx]->setRetryReq();
                status = false;
            }
            else {
                ofenceTotal++;
                perThreadETs[idx]->serviceOFence();
                if (pkt->needsResponse()) {
                    pkt->makeResponse();
                    slavePorts[idx]->schedTimingResp(pkt, curTick());
                }
                status = true;
            }
        }
        // Acquire
        else if (pkt->req->isAcquire()) {
            if (useARP) {
                Addr lockAddr = alignAddr(pkt->getAddr());
                std::map<Addr, depPair>::iterator itr=lock_map.find(lockAddr);

                DPRINTF(PersistBuffer, "Acquire at core %d, Addr=0x%lx\n", idx, lockAddr);
                // Cross thread dependency exists
                if (itr != lock_map.end() && (itr->second).first != idx) {
                    int srcCoreID = (itr->second).first;

                    if (perThreadETs[idx]->getSize() == etCapacity) {
                        perThreadETs[idx]->setRetryReq();
                        status = false;
                    }
                    else {
                        timestamp srcEpoch =
                            perThreadETs[srcCoreID]->addDepThread(idx);
                        if (srcEpoch != -1) {
                            perThreadETs[idx]->createNewEpoch();
                            perThreadETs[idx]->addCrossThreadDep(srcCoreID, srcEpoch);
                            interTEpochConflict++;
                            DPRINTF(PersistBuffer, "Cross thread dep: Source:%d, %d \
                                Destination: %d, %d\n", srcCoreID, srcEpoch, idx,
                                perThreadETs[idx]->getCurrentTS());
                        }
                        status = true;
                    }
                }
                else {
                    status = true;
                }
            }
            else { // Epoch persistency
                status = true;
            }

             if (status) {
                if (pkt->needsResponse()) {
                    pkt->makeResponse();
                    slavePorts[idx]->schedTimingResp(pkt, curTick());
                }
            }
        }
        // Release
        else if (pkt->req->isRelease()) {
            if (useARP) {
                Addr lockAddr = alignAddr(pkt->getAddr());
                DPRINTF(PersistBuffer, "Release at core %d, Addr=0x%lx\n", idx, lockAddr);

                if (perThreadETs[idx]->getSize() == etCapacity) {
                    perThreadETs[idx]->setRetryReq();
                    status = false;
                }
                else {
                    // Update the lock map with this thread ID
                    std::map<Addr, depPair>::iterator itr;
                    itr = lock_map.find(lockAddr);
                    if (itr == lock_map.end())
                        lock_map.insert(std::pair<Addr, depPair>(lockAddr,
                                depPair(idx, perThreadETs[idx]->getCurrentTS())));
                    else {
                        (itr->second).first = idx;
                        (itr->second).second = perThreadETs[idx]->getCurrentTS();
                    }

                    // Create a new epoch
                    perThreadETs[idx]->createNewEpoch();
                    status = true;
                }
            }
            else { // Epoch persistency
                status = true;
            }

            if (status) {
                if (pkt->needsResponse()) {
                    pkt->makeResponse();
                    slavePorts[idx]->schedTimingResp(pkt, curTick());
                }
            }
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
                    if (perThreadPBs[idx]->getSize() == pbCapacity) {
                        status = false;
                        DPRINTF(PersistBuffer, "Core%d: PB stall\n", idx);
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
                    if (perThreadPBs[idx]->getSize() >= flushThreshold) {
                        if (!perThreadPBs[idx]->flushEvent.scheduled())
                            schedule(perThreadPBs[idx]->flushEvent,
                                    nextCycle());
                    }
                }
            }
            else { // Assumed to be PIO address
                status = masterPorts[idx]->sendTimingReq(pkt);
            }
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
            if (pkt->req->isNVM() &&
                bufferedAddr.find(alignAddr(pkt->getAddr()))
                != bufferedAddr.end()) {
                // TODO: Need to handle early evictions
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

    /* Sink PM memory EC and write responses here itself */
    if (pkt->cmd == MemCmd::EpochCompResp && idx >= numThreads) {
        SenderInfo *s = dynamic_cast<SenderInfo *>(pkt->senderState);
        perThreadETs[s->coreID]->
            handleEpochCompletion(s->TS, (int)idx-numThreads);
        pkt->req.reset();
        delete pkt->popSenderState();
        delete pkt;
        status = true;

        if (!perThreadETs[s->coreID]->ETCycle.scheduled())
            schedule(perThreadETs[s->coreID]->ETCycle, curTick());
    }
    else if (pkt->req->isNVM() && pkt->isWrite() && idx >= numThreads &&
        !pkt->req->isPMFence()) {
        SenderInfo *s = dynamic_cast<SenderInfo *>(pkt->senderState);

        if (pkt->cmd == MemCmd::UTFullError) {
            DPRINTF(PersistBuffer, "Core%d: Received NACK Addr: 0x%lx\n",
                    s->coreID, pkt->getAddr());
            perThreadPBs[s->coreID]->handleUTFullError(pkt);
            pkt->req.reset();
            delete pkt->popSenderState();
            delete pkt;
            status = true;
        }
        else {
            perThreadPBs[s->coreID]->flushAck(pkt);

            pkt->req.reset();
            delete pkt->popSenderState();
            delete pkt;
            status = true;
        }
    }
    else if (pkt->isWrite() &&
            idx >= numThreads && pkt->req->isDMA()) { // DMA Write
        status = slavePorts[idx]->sendTimingResp(pkt);
    }
    else {
        status = slavePorts[idx]->sendTimingResp(pkt);
    }

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
            pb.perThreadETs[srcCore]->createNewEpoch(); // Create a new epoch at src
            timestamp srcEpoch = pb.perThreadETs[srcCore]->addDepThread(id);
            DPRINTF(PersistBuffer, "Cross thread dep: Source:%d, %d \
                            Destination: %d\n", srcCore, srcEpoch, id);
            et.createNewEpoch();
            et.addCrossThreadDep(srcCore, srcEpoch);
            pb.interTEpochConflict++;
            it->second = id; // Update the directory
        }
    }
    else {
        pb.simpleDir.insert(std::pair<Addr,PortID>(addr,id));
    }
}

bool
CentralPersistBuffer::PersistBuffer::tryCoalescePBEntry(PacketPtr pkt)
{
    bool coalesced = false;
    timestamp currTS = et.getCurrentTS();
    pbOccupancy.sample(PBEntries.size());

    if (pb.bufferedAddr.find(pb.alignAddr(pkt->getAddr()))
            != pb.bufferedAddr.end()) {
        //Coalesce stores if possible
        for (auto entry : PBEntries){
            // Coalesce only if stores are in the same epoch
            if (entry->getAddr() == pb.alignAddr(pkt->getAddr())
                    && entry->getTS() == currTS
                    && entry->getFlushStatus() != FLUSHED) {
                coalesced = true;
            }
        }
    }
    return coalesced;
}

bool
CentralPersistBuffer::PersistBuffer::coalescePBEntry(PacketPtr pkt)
{
    bool coalesced = false;
    timestamp currTS = et.getCurrentTS();

    if (pb.bufferedAddr.find(pb.alignAddr(pkt->getAddr()))
            != pb.bufferedAddr.end()) {
        //Coalesce stores if possible
        for (auto entry : PBEntries){
            // Coalesce only if stores are in the same epoch
            if (entry->getAddr() == pb.alignAddr(pkt->getAddr())
                    && entry->getTS() == currTS
                    && entry->getFlushStatus() != FLUSHED) {
                pkt->writeDataToBlock(entry->getDataPtr(), BLK_SIZE);
                entry->setMask(pkt->getOffset(BLK_SIZE),
                                pkt->getSize());
                et.numCoalesced++;
                et.epochEntries++;
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
    DPRINTF(PersistBuffer, "PB%d: Added entry for addr: 0x%x\n", id, pkt->getAddr());
    timestamp currTS = et.getCurrentTS();
    PBEntry *newEntry = new PBEntry(pkt, currTS);
    newEntry->setMask(pkt->getOffset(BLK_SIZE), pkt->getSize());
    PBEntries.push_back(newEntry);

    //Update the bufferedAddr list()
    pb.bufferedAddr.insert(pb.alignAddr(pkt->getAddr()));

    et.incPendingACKs();
    unflushedEntries++;

    // Stats
    pb.entriesInserted++;
    et.epochEntries++;
}

void
CentralPersistBuffer::PersistBuffer::flushPB() {
    writeType type;
    int mc = -1;
    PBEntry *flushEntry = getOldestUnflushed();

    if (flushEntry == NULL) {
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

    if (et.isFlushSafe(flushEntry->getTS(), mc-pb.numThreads)) {
        if (flushEntry->getFlushStatus() == NACKED) {
            type = RETRY;
            stopSpecFlush = false;
            et.clearMcSpecMask(flushEntry->getTS(), mc-pb.numThreads);
            DPRINTF(PersistBuffer, "Core%d Retrying Addr=0x%lx\n", id, flushEntry->getAddr());
        }
        else
            type = SAFE;
    }
    else {
        if (stopSpecFlush) {
            noflushCycles++;
            return;
        }
        else
            type = SPECULATIVE;
    }

    if (type == SPECULATIVE) {
        if (et.checkMcMask(flushEntry->getTS(), mc-pb.numThreads)) {
            type = SAFE;
        }
    }

    RequestPtr req = std::make_shared<Request>(flushEntry->getAddr(),
            flushEntry->getSize(), 0, pb.masterId);
    req->setFlags(Request::NVM);

    PacketPtr pkt = Packet::createWrite(req);

    uint8_t *newData = new uint8_t[BLK_SIZE];
    pkt->dataDynamic(newData);
    memcpy(newData, flushEntry->getDataPtr(), BLK_SIZE);
    pkt->setMask(flushEntry->getMask());

    SenderInfo *s = new SenderInfo(id, flushEntry->getTS(), type);
    pkt->pushSenderState(s);

    bool flushed = pb.masterPorts[mc]->sendTimingReq(pkt);
    if (flushed) {
        flushEntry->setFlushStatus(FLUSHED);
        unflushedEntries--;
        et.setMcWriteMask(flushEntry->getTS(), mc-pb.numThreads);
        if (type == SPECULATIVE)
            et.setMcSpecMask(flushEntry->getTS(), mc-pb.numThreads);
    }
    else {
        pb.memSaturated[mc-pb.numThreads] = true;
        if (pb.bwSatStart[mc-pb.numThreads] == 0)
            pb.bwSatStart[mc-pb.numThreads]= curTick();
        DPRINTF(PersistBuffer, "BW saturated at MC %d\n", mc);
        pkt->req.reset();
        delete pkt->popSenderState();
        delete pkt;
    }
}

void
CentralPersistBuffer::PersistBuffer::flushAck(PacketPtr pkt){

    timestamp entryTS = -1;

    SenderInfo *s = dynamic_cast<SenderInfo *>(pkt->senderState);

    for (int i=0; i< PBEntries.size(); ++i) {
        PBEntry *flushEntry = PBEntries[i];
        if (flushEntry->getAddr() == pkt->getAddr()
            && flushEntry->getTS() == s->TS
            && flushEntry->getFlushStatus() == FLUSHED) {
                entryTS = s->TS;
                delete flushEntry;
                PBEntries.erase(PBEntries.begin()+i);
                pb.entriesFlushed++;
                break;
        }
    }

    et.decPendingACKs(entryTS);
    if (!et.ETCycle.scheduled())
        schedule(et.ETCycle, curTick());

    // Hack for removing one copy of addr from buffer
    Addr a =pb.alignAddr(pkt->getAddr());
    if (pb.bufferedAddr.find(a) != pb.bufferedAddr.end()) {
        auto location = pb.bufferedAddr.find(
                pb.alignAddr(pkt->getAddr()));
        pb.bufferedAddr.erase(location);

        /*
        auto l2 = pb.bufferedAddr.find(a);
        if (l2 == pb.bufferedAddr.end()) {
            auto it = pb.simpleDir.find(a);
            if (it != pb.simpleDir.end())
                pb.simpleDir.erase(it);
            else
                assert(false);
        }
        */
    }
    //Shouldn't be here, if an addr is buffered,
    // it should be present
    else assert(false);

    /* treshold based signalling*/
    if (retryWrReq) {
        retryWrReq = false;
        assert(stallStart > 0);
        stallCycles += (curTick() - stallStart);
        stallStart = 0;
        pb.slavePorts[id]->sendRetryReq();
    }
}

void CentralPersistBuffer::PersistBuffer::handleUTFullError(PacketPtr pkt)
{
    SenderInfo *s = dynamic_cast<SenderInfo *>(pkt->senderState);

    if (!stopSpecFlush) {
        numNackBursts++;
        // Stop flushing speculative stores since UT full
        stopSpecFlush = true;
    }

    for (auto entry: PBEntries) {
        if (entry->getAddr() == pkt->getAddr() &&
            entry->getTS() == s->TS &&
            entry->getFlushStatus() == FLUSHED) {
                entry->setFlushStatus(NACKED);
                break;
            }
    }
    unflushedEntries++;

    if (!flushEvent.scheduled())
        schedule(flushEvent, pb.nextCycle());
}

void CentralPersistBuffer::PersistBuffer::processFlushEvent(){
    if (unflushedEntries) { /* PB has buffered entries waiting to be flushed */
        flushPB();
        if (dFenceInProg) {
            if (unflushedEntries > 0) {
                if (!flushEvent.scheduled())
                    schedule(flushEvent, pb.nextCycle());
            }
        } else if (unflushedEntries >= pb.flushThreshold) {
            if (!flushEvent.scheduled())
                schedule(flushEvent, pb.nextCycle());
        }
    }
}

void CentralPersistBuffer::EpochTable::
        handleEpochCompletion(timestamp TS, PortID mcNum) {
    for (int i=0; i<epochs.size(); i++) {
        if (epochs[i]->getTS() == TS) {
            epochs[i]->clearMcSpecMask(mcNum);
            // All MCs acknowledged EC, so delete epoch
            if (!epochs[i]->getHasSpecWrites())
            {
                PortID depThread = epochs[i]->dependentThread;
                if (depThread != -1) {
                    // send msg to dependent Threads
                    cpb.perThreadETs[depThread]->addECMsg(coreID, TS);
                }
                DPRINTF(PersistBuffer, "Core%d Epoch:%d complete\n", coreID, TS);
                delete epochs[i];
                epochs.erase(epochs.begin()+i);

                if (retryPkt) {
                    cpb.slavePorts[coreID]->sendRetryReq();
                    retryPkt = false;

                    cpb.perThreadPBs[coreID]->etStallCycles += curTick() -
                        etStallStart;
                    etStallStart = 0;
                }
            }
            break;
        }
    }
}

void CentralPersistBuffer::EpochTable::processETCycle() {
    //TODO: Send 1 message for all completed epochs
    for (auto entry: epochs) {
        if (entry->getPendingACKs() != 0 ||
            entry->crossThreadDep.first != -1)
            break;
        else if (!(entry->getIsFlushing())) {
            if (entry->getHasSpecWrites()) {
                entry->setIsFlushing(true);

                std::vector <bool> mask = entry->mcSpecMask;
                for (int i=0; i<mask.size(); i++) {
                    if (mask[i]) {
                        RequestPtr req = std::make_shared<Request>();
                        PacketPtr pkt = Packet::createEpochCompletion(req);
                        SenderInfo *s = new SenderInfo(coreID,
                            entry->getTS(), INVALID);
                        pkt->pushSenderState(s);

                        cpb.masterPorts[i+cpb.numThreads]->sendTimingReq(pkt);
                    }
                    // Stores to same MC become safe because in ordered delivery
                    safeTS[i] = _max(safeTS[i], entry->getTS() + 1);
                }
            }
            else {
                if (entry == epochs.front()) {
                    for (int i=0; i<cpb.numMCs; ++i)
                        safeTS[i] = _max(safeTS[i], entry->getTS() + 1);
                    handleEpochCompletion(entry->getTS(), 0);
                }
            }
            //break; // TODO: Need break??
        }
    }

    if (dfenceInProg) {
        if (epochs.size() == 0) {
            assert(dfenceStart > 0);
            cpb.perThreadPBs[coreID]->dfenceCycles += curTick() - dfenceStart;
            dfenceStart = 0;
            PacketPtr respPkt = dfencePkts.front();
            if (respPkt->needsResponse())
                respPkt->makeResponse();
            bool status = cpb.recvTimingResp(respPkt, coreID);
            if (status) {
                dfenceInProg = false;
                dfencePkts.pop_front();
                DPRINTF(PersistBuffer, "PB%d: DFENCE complete\n", coreID);
            } else {
                DPRINTF(PersistBuffer, "PB%d: DFENCE response failed\n", coreID);
            }
        }
    }

    //if (dfenceInProg) {
    //    if (!ETCycle.scheduled())
    //        schedule(ETCycle, cpb.nextCycle());
    //}
}

void CentralPersistBuffer::EpochTable::processECEvent() {
    assert (ecMsgs.size() != 0); // event scheduled only if msg added

    depPair dep = ecMsgs.front();
    ecMsgs.pop_front();
    bool entry_found = false;
    for (auto entry: epochs) {
        if (entry->crossThreadDep == dep) {
            entry->crossThreadDep.first = -1;
            entry_found = true;
            DPRINTF(PersistBuffer, "Cross thread dep resolved: Source:%d, %d \
                Destination:%d, %d\n", dep.first, dep.second, coreID, entry->getTS());
            break;
        }
    }
    // Current epoch dependency resolved
    if (!entry_found) {
        if (currentEpoch->crossThreadDep == dep) {
            currentEpoch->crossThreadDep.first = -1;
            entry_found = true;
            DPRINTF(PersistBuffer, "Cross thread dep resolved: Source:%d, %d \
                Destination: %d, %d\n", dep.first, dep.second, coreID, getCurrentTS());
        }
    }
    assert(entry_found);


    if (ecMsgs.size() != 0) {
        if (!ECEvent.scheduled())
            schedule(ECEvent, cpb.nextCycle());
    }

    if (!ETCycle.scheduled())
        schedule(ETCycle, curTick());
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
        .desc("Cycles flushing stopped due to saturated BW");
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

    etOccupancy
        .init(pb.etCapacity+1)
        .name(name() + ".etOccupancy")
        .desc("Occupancy of perThread ETs")
        .flags(nonan);

    wawHits
         .init(pb.pbCapacity+1)
         .name(name() + ".wawHits")
         .desc("WAW reuse in an epoch")
         .flags(nozero);

    stallCycles
        .name(name() + ".cyclesStalled")
        .desc("Number of cycles stalled due to full PB");

    etStallCycles
        .name(name() + ".etStalled")
        .desc("Number of cycles stalled due to full ET");

    dfenceCycles
        .name(name() + ".dfenceStalled")
        .desc("Number of cycles stalled due to dfence");

    noflushCycles
        .name(name() + ".cyclesBlocked")
        .desc("Number of cycles per PB stalled due to flushing block");

    numNackBursts
        .name(name() + ".numNackBursts")
        .desc("Number of times NACK received because of full URT");
}
