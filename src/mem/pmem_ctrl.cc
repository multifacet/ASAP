/* Authors: Swapnil Haria
 */

#include "mem/pmem_ctrl.hh"

#include "base/bitfield.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/Drain.hh"
#include "debug/PMEM.hh"
#include "debug/PMEMDebug.hh"
#include "sim/system.hh"

using namespace std;

PMEMCtrl::PMEMCtrl(const PMEMCtrlParams* p) :
    AbstractMemory(p),
    port(name() + ".port", *this), retryRdReq(false),
    retryWrReq(false), hasPWQ(p->pwq),
    readBandwidth(p->read_bandwidth), writeBandwidth(p->write_bandwidth),
    readLatency(p->read_latency), writeLatency(p->write_latency),
    readProcessingLatency(64*p->read_bandwidth),
    writeProcessingLatency(64*p->write_bandwidth),
    frontendLatency(p->frontend_latency), burstLength(p->burst_length),
    channels(p->channels), numCores(p->num_cores),
    URTCapacity(p->urt_capacity),
    nextReqEvent([this]{ processNextReqEvent(); }, name()),
    isBusy(false), memSchedPolicy(p->mem_sched_policy),
    addrMapping(p->addr_mapping), pageMgmt(p->page_policy),
    elidedWrite(p->elided_write), elidedWriteCount(0)
{
    /* According to little's law, outstanding req = latency * throughput.
     * However, bandwidth stored in ticks per byte and latency in ticks.
     * Hence we divide by bandwidth here
     */

    // TODO: Figure out the queue size
    readBufferSize = 16;
    writeBufferSize = 16;

    writeHighThreshold = writeBufferSize * p->write_high_thresh_perc/100;
    writeLowThreshold = writeBufferSize * p->write_low_thresh_perc/100;

    nackPkts = std::vector<bool>(numCores, false);

    DPRINTF(PMEM, "ReadBufferSize:%" PRIu32 " WriteBufferSize:%" PRIu32 " \n",
            readBufferSize, writeBufferSize);
    DPRINTF(PMEM, "WriteHighThresh:%" PRIu32 " WriteLowThresh:%" PRIu32 " \n",
            writeHighThreshold, writeLowThreshold);
    DPRINTF(PMEM, "WriteBandwidth::%f WriteLatency:%" PRIu64 " \n",
            writeBandwidth, writeLatency);
    DPRINTF(PMEM, "ReadBandwidth::%f ReadLatency:%" PRIu64 " \n",
            readBandwidth, readLatency);
}

void
PMEMCtrl::init()
{
    AbstractMemory::init();

    if (!port.isConnected()) {
        fatal("PMEMCtrl %s is unconnected!\n", name());
    } else {
        port.sendRangeChange();
    }
}

Tick
PMEMCtrl::recvAtomic(PacketPtr pkt)
{
    DPRINTF(PMEM, "recvAtomic: %s 0x%x\n", pkt->cmdString(), pkt->getAddr());

    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    // Access backing store and turns packet into a response
    access(pkt);

    return frontendLatency + pkt->isRead()?readLatency:writeLatency;
}

void
PMEMCtrl::recvFunctional(PacketPtr pkt)
{
    // rely on the abstract memory
    functionalAccess(pkt);
}

bool
PMEMCtrl::readQueueFull(unsigned int neededEntries) const
{
    DPRINTF(PMEM, "Read queue limit %d, current size %d, entries needed %d\n",
            readBufferSize, readQueue.size() + readRespQueue.size(),
            neededEntries);

    return
        (readQueue.size() + readRespQueue.size() + neededEntries)
            > readBufferSize;
}

bool
PMEMCtrl::writeQueueFull(unsigned int neededEntries) const
{
    DPRINTF(PMEM, "Write queue limit %d, current size %d, entries needed %d\n",
            writeBufferSize, writeQueue.size() + writeRespQueue.size(),
            neededEntries);
    return (writeQueue.size() + writeRespQueue.size() + neededEntries)
            > writeBufferSize;
}
/*
PMEMCtrl::PMEMPacket*
PMEMCtrl::decodeAddr(PacketPtr pkt, Addr pmemPktAddr, unsigned size,
                       bool isRead)
{
    // decode the address based on the address mapping scheme, with
    // Ro, Ra, Co, Ba and Ch denoting row, rank, column, bank and
    // channel, respectively
    uint8_t rank;
    uint8_t bank;
    // use a 64-bit unsigned during the computations as the row is
    // always the top bits, and check before creating the PMEMPacket
    uint64_t row;

    // truncate the address to a PMEM burst, which makes it unique to
    // a specific column, row, bank, rank and channel
    Addr addr = pmemPktAddr / burstLength;

    // we have removed the lowest order address bits that denote the
    // position within the column
    if (addrMapping == Enums::RoRaBaCoCh) {
        // take out the lower-order column bits
        addr = addr / columnsPerStripe;

        // take out the channel part of the address
        addr = addr / channels;

        // next, the higher-order column bites
        addr = addr / (columnsPerRowBuffer / columnsPerStripe);

        // after the column bits, we get the bank bits to interleave
        // over the banks
        bank = addr % banksPerRank;
        addr = addr / banksPerRank;

        // after the bank, we get the rank bits which thus interleaves
        // over the ranks
        rank = addr % ranksPerChannel;
        addr = addr / ranksPerChannel;

        // lastly, get the row bits, no need to remove them from addr
        row = addr % rowsPerBank;
    } else
        panic("Unknown address mapping policy chosen!");

    DPRINTF(PMEM, "Address: %x Rank %d Bank %d Row %d\n",
            pmemPktAddr, rank, bank, row);

    // create the corresponding PMEM packet with the entry time and
    // ready time set to the current tick, the latter will be updated
    // later
    uint16_t bank_id = banksPerRank * rank + bank;
    return new PMEMPacket(pkt, isRead, rank, bank, row, bank_id, pmemPktAddr,
                          size, ranks[rank]->banks[bank], *ranks[rank]);
}
*/

void
PMEMCtrl::scheduleWrite()
{
    if (elidedWrite == 0) {
        schedule(nextReqEvent, curTick() + writeProcessingLatency);
        nonElidedWriteReqs++;
    }
    else {
        elidedWriteCount++;
        if (elidedWriteCount==elidedWrite) {
            elidedWriteCount = 0;
            schedule(nextReqEvent, curTick() + 1);
        } else {
            schedule(nextReqEvent, curTick() + writeProcessingLatency);
            nonElidedWriteReqs++;
        }
    }
}

void
PMEMCtrl::addToReadQueue(PacketPtr pkt, unsigned int pktCount)
{
    // only add to the read queue here. whenever the request is
    // eventually done, set the readyTime, and call schedule()
    assert(!pkt->isWrite());

    assert(pktCount != 0);

    // if the request size is larger than burst size, the pkt is split into
    // multiple PMEM packets
    // Note if the pkt starting address is not aligened to burst size, the
    // address of first PMEM packet is kept unaliged. Subsequent PMEM packets
    // are aligned to burst size boundaries. This is to ensure we accurately
    // check read packets against packets in write queue.
    Addr addr = pkt->getAddr();
    unsigned pktsServicedByWrQ = 0;
    BurstHelper* burst_helper = NULL;
    for (int cnt = 0; cnt < pktCount; ++cnt) {
        unsigned size = std::min((addr | (burstLength - 1)) + 1,
                        pkt->getAddr() + pkt->getSize()) - addr;
        readPktSize[ceilLog2(size)]++;
        readBursts++;

        // First check write buffer to see if the data is already at
        // the controller
        bool foundInWrQ = false;
        Addr burst_addr = burstAlign(addr);
        // if the burst address is not present then there is no need
        // looking any further
        if (isInWriteQueue.find(burst_addr) != isInWriteQueue.end()) {
            for (const auto& p : writeQueue) {
                // check if the read is subsumed in the write queue
                // packet we are looking at
                if (p->addr <= addr && (addr + size) <= (p->addr + p->size)) {
                    foundInWrQ = true;
                    servicedByWrQ++;
                    pktsServicedByWrQ++;
                    DPRINTF(PMEM, "Read to addr %x with size %d serviced by "
                            "write queue\n", addr, size);
                    bytesReadWrQ += burstLength;
                    break;
                }
            }
        }

        // If not found in the write q, make a PMEM packet and
        // push it onto the read queue
        if (!foundInWrQ) {

            // Make the burst helper for split packets
            if (pktCount > 1 && burst_helper == NULL) {
                DPRINTF(PMEM, "Read to addr %lx translates to %d "
                        "PMEM requests\n", pkt->getAddr(), pktCount);
                burst_helper = new BurstHelper(pktCount);
            }

            PMEMPacket* pmem_pkt = new PMEMPacket(pkt, addr, size);
            pmem_pkt->burstHelper = burst_helper;

            assert(!readQueueFull(1));
            //rdQLenPdf[readQueue.size() + readRespQueue.size()]++;

            DPRINTF(PMEM, "Adding to read queue\n");

            readQueue.push_back(pmem_pkt);

            // Update stats
            avgRdQLen = readQueue.size() + readRespQueue.size();
        }

        // Starting address of next PMEM pkt (aligend to burstLength boundary)
        addr = (addr | (burstLength - 1)) + 1;
    }

    // If all packets are serviced by write queue, we send the repsonse back
    if (pktsServicedByWrQ == pktCount) {
        // do the actual memory access which also turns the packet into a
        // response
        access(pkt);
        respond(pkt, 0);
        return;
    }

    // Update how many split packets are serviced by write queue
    if (burst_helper != NULL)
        burst_helper->burstsServiced = pktsServicedByWrQ;

    // If we are not already scheduled to get a request out of the
    // queue, do so now
    if (!nextReqEvent.scheduled()) {
        DPRINTF(PMEM, "Request to be processed immediately\n");
        busState = READ;
        schedule(nextReqEvent, curTick() + readProcessingLatency);
    }
}

void
PMEMCtrl::addToWriteQueue(PacketPtr pkt, unsigned int pktCount)
{
    // only add to the write queue here. whenever the request is
    // eventually done, set the readyTime, and call schedule()
    assert(pkt->isWrite());

    assert(pktCount != 0);
    // if the request size is larger than burst size, the pkt is split into
    // multiple PMEM packets
    Addr addr = pkt->getAddr();
    BurstHelper* burst_helper = NULL;
    for (int cnt = 0; cnt < pktCount; ++cnt) {
        unsigned size = std::min((addr | (burstLength - 1)) + 1,
                        pkt->getAddr() + pkt->getSize()) - addr;
        writePktSize[ceilLog2(size)]++;
        writeBursts++;

        // Try to merge only if WQ is persistent
        // Else it will be difficult to respond to each of the merging reqs
        bool merged = false;
        if (hasPWQ) {
            merged = isInWriteQueue.find(burstAlign(addr)) !=
                        isInWriteQueue.end();
        }

        // if the item was not merged we need to create a new write
        // and enqueue it
        if (!merged) {
            // Make the burst helper for split packets
            if (pktCount > 1 && burst_helper == NULL) {
                DPRINTF(PMEM, "Write to addr %lx translates to %d "
                        "PMEM requests\n", pkt->getAddr(), pktCount);
                burst_helper = new BurstHelper(pktCount);
            }

            PMEMPacket* pmem_pkt = new PMEMPacket(pkt, addr, size);
            if (hasPWQ) {
                PacketPtr new_pkt = Packet::createWrite(pkt->req);
                pmem_pkt->pkt = new_pkt;
            }
            pmem_pkt->burstHelper = burst_helper;

            assert(writeQueue.size() < writeBufferSize);
            //wrQLenPdf[writeQueue.size() + writeRespQueue.size()]++;

            DPRINTF(PMEM, "Adding to write queue\n");

            writeQueue.push_back(pmem_pkt);
            isInWriteQueue.insert(burstAlign(addr));

            // Update stats
            avgWrQLen = writeQueue.size() + writeRespQueue.size();
        } else {
            DPRINTF(PMEM, "Merging write burst with existing queue entry\n");

            // keep track of the fact that this burst effectively
            // disappeared as it was merged with an existing one
            mergedWrBursts++;
        }

        // Starting address of next PMEM pkt (aligend to burstLength boundary)
        addr = (addr | (burstLength - 1)) + 1;
    }

    // If write queue is persistent, we respond immediately. Actual memory
    // access will be performed later, as per MC scheduling policies.
    // Any later reads can simply snoop the updated value from the write queue
    if (hasPWQ && pkt->needsResponse()) {
        access(pkt);
        respond(pkt, 0);
    } else {
        // We perform the access at time of insertion so that reads
        // that get a value from the write queue can directly read memory.
        access(pkt);
    }

    // If we are not already scheduled to get a request out of the
    // queue, do so now
    if (!writeQueue.empty() && !nextReqEvent.scheduled()) {
        DPRINTF(PMEM, "Request to be processed immediately\n");
        busState = WRITE;
        scheduleWrite();
//        schedule(nextReqEvent, curTick() + writeLatency);
    }
}

void
PMEMCtrl::addReadWriteRequest(PacketPtr pkt, unsigned int pktCount,
                            URRecord* rec)
{
    assert(pkt->isWrite());

    assert(pktCount != 0);
    // if the request size is larger than burst size, the pkt is split into
    // multiple PMEM packets
    Addr addr = pkt->getAddr();
    BurstHelper* burst_helper = NULL;
    for (int cnt = 0; cnt < pktCount; ++cnt) {
        unsigned size = std::min((addr | (burstLength - 1)) + 1,
                        pkt->getAddr() + pkt->getSize()) - addr;
        writePktSize[ceilLog2(size)]++;
        writeBursts++;

        // Make the burst helper for split packets
        if (pktCount > 1 && burst_helper == NULL) {
            DPRINTF(PMEM, "Write to addr %lx translates to %d "
                    "PMEM requests\n", pkt->getAddr(), pktCount);
            burst_helper = new BurstHelper(pktCount);
        }

        PMEMPacket* pmem_pkt = new PMEMPacket(pkt, addr, size);
        if (hasPWQ) {
            PacketPtr new_pkt = Packet::createWrite(pkt->req);
            pmem_pkt->pkt = new_pkt;
        }
        pmem_pkt->burstHelper = burst_helper;

        assert(writeQueue.size() < writeBufferSize);
        //wrQLenPdf[writeQueue.size() + writeRespQueue.size()]++;

        DPRINTF(PMEM, "Adding to write queue\n");

        writeQueue.push_back(pmem_pkt);
        isInWriteQueue.insert(burstAlign(addr));

        // Update stats
        avgWrQLen = writeQueue.size() + writeRespQueue.size();

        // Starting address of next PMEM pkt (aligend to burstLength boundary)
        addr = (addr | (burstLength - 1)) + 1;
    }

    // If write queue is persistent, we respond immediately. Actual memory
    // access will be performed later, as per MC scheduling policies.
    // Any later reads can simply snoop the updated value from the write queue
    pkt->cmd = MemCmd::SwapReq;
    access(pkt);
    pkt->cmd = MemCmd::WriteReq;
    memcpy(rec->data, pkt->getConstPtr<uint8_t>(), BLK_SIZE);
    if (hasPWQ && pkt->needsResponse()) {
        pkt->makeResponse(); // access() makes SwapResp response
        respond(pkt, 0);
    }

    // If we are not already scheduled to get a request out of the
    // queue, do so now
    if (!writeQueue.empty() && !nextReqEvent.scheduled()) {
        DPRINTF(PMEM, "Request to be processed immediately\n");
        busState = WRITE;
        scheduleWrite();
//        schedule(nextReqEvent, curTick() + writeLatency);
    }
}

void
PMEMCtrl::printQs() const {
    DPRINTF(PMEM, "===READ QUEUE===\n\n");
    for (auto i = readQueue.begin() ;  i != readQueue.end() ; ++i) {
        DPRINTF(PMEM, "Read %lu\n", (*i)->addr);
    }
    DPRINTF(PMEM, "\n===READ RESP QUEUE===\n\n");
    for (auto i = readRespQueue.begin() ;  i != readRespQueue.end() ; ++i) {
        DPRINTF(PMEM, "Response %lu\n", (*i)->addr);
    }
    DPRINTF(PMEM, "\n===WRITE QUEUE===\n\n");
    for (auto i = writeQueue.begin() ;  i != writeQueue.end() ; ++i) {
        DPRINTF(PMEM, "Write %lu\n", (*i)->addr);
    }
    DPRINTF(PMEM, "\n===WRITE RESP QUEUE===\n\n");
    for (auto i = writeRespQueue.begin() ;  i != writeRespQueue.end() ; ++i) {
        DPRINTF(PMEM, "Response %lu\n", (*i)->addr);
    }
}

void
PMEMCtrl::handleEpochCompletion(PortID core, timestamp TS)
{
    for (int i=0; i<URTable.size(); i++) {
        URRecord *del_entry = URTable[i];
        if (del_entry->coreID == core && del_entry->TS == TS) {
            // Apply Redo record to Undo
            if (del_entry->type == REDO) {
                bool undo_found = false;
                for (auto record: URTable) {
                    if (record->addr == del_entry->addr && record->type == UNDO) {
                        bool overlap = false;
                        undo_found = true;
                        totRedoUndoAlias++;
                        for (int i = 0; i < BLK_SIZE; i++) {
                            if (del_entry->mask[i]) {
                                if (del_entry->mask[i] && record->mask[i])
                                    overlap = true;
                                record->data[i] = del_entry->data[i];
                                record->mask[i] = true;
                            }
                        }
                        // Update memory if writing to different part of cacheline
                        if (!overlap)
                            undo_found = false;
                        break;
                    }
                }
                if (!undo_found) {
                    // TODO: Add to the write queue instead of directly updating
                    RequestPtr req = std::make_shared<Request>(del_entry->addr,
                            del_entry->size, 0, 0);
                    req->setFlags(Request::NVM);

                    PacketPtr pmem_pkt = Packet::createWrite(req);
                    uint8_t *newData = new uint8_t[BLK_SIZE];
                    pmem_pkt->dataDynamic(newData);
                    memcpy(newData, del_entry->data, BLK_SIZE);
                    pmem_pkt->setMask(del_entry->mask);

                    access(pmem_pkt);

                    pmem_pkt->req.reset();
                    delete pmem_pkt;
                }
            }
            DPRINTF(PMEMDebug, "Deleted record type=%d Core%d TS=%d Addr:0x%lx\n",
                    del_entry->type, del_entry->coreID, del_entry->TS, del_entry->addr);
            delete del_entry;
            URTable.erase(URTable.begin()+i);
            i--;
        }
    }
}

bool
PMEMCtrl::recvTimingReq(PacketPtr pkt)
{
    if (pkt->isOwnershipLost()) {
            delete pkt;
            return true;
    }

    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    panic_if(!(pkt->isRead() || pkt->isWrite()
             || pkt->cmd == MemCmd::EpochCompReq),
             "Should only see reads, writes and EC at memory controller, "
             "saw %s to %#llx\n", pkt->cmdString(), pkt->getAddr());

    // Find out how many pmem packets a pkt translates to
    // If the burst length is equal or larger than the pkt size, then a pkt
    // translates to only one PMEM packet. Otherwise, a pkt translates to
    // multiple PMEM packets
    unsigned size = pkt->getSize();
    unsigned offset = pkt->getAddr() & (burstLength - 1);
    unsigned int pmem_pkt_count = divCeil(offset + size, burstLength);

    // check local buffers and do not accept if full
    if (pkt->isRead()) {
        assert(size != 0);
        if (readQueueFull(pmem_pkt_count)) {
            DPRINTF(PMEM, "Read queue full, not accepting\n");
            // remember that we have to retry this port
            retryRdReq = true;
            numRdRetry++;
            return false;
        } else {
            addToReadQueue(pkt, pmem_pkt_count);
            readReqs++;
            bytesReadSys += size;
        }
    } else {
        assert(pkt->isWrite());
        assert(size != 0);
        if (writeQueueFull(pmem_pkt_count)) {
            DPRINTF(PMEM, "Write queue full, not accepting\n");
            // remember that we have to retry this port
            retryWrReq = true;
            numWrRetry++;
            return false;
        } else {
            addToWriteQueue(pkt, pmem_pkt_count);
            writeReqs++;
            bytesWrittenSys += size;
        }
    }
    return true;
}

void
PMEMCtrl::respond(PacketPtr pkt, Tick queue_delay)
{
    DPRINTF(PMEM, "Responding to Address %x.. \n",pkt->getAddr());

    // No response is generated for these packets so we handle them separately
    if (pkt->cmd == MemCmd::CleanEvict || pkt->cmd == MemCmd::WritebackClean) {
        DPRINTF(PMEM, "Done\n");
        return;
    }

    // access already turned the packet into a response
    assert(pkt->isResponse());
    // response_time incorporates headerDelay (has any xbar delay)
    // and payloadDelay (accounts for number of data beats)
    Tick response_time;
    if (pkt->isWrite())
        response_time = curTick() + frontendLatency + pkt->headerDelay +
            pkt->payloadDelay + writeLatency;
    else if (pkt->cmd == MemCmd::EpochCompResp)
        response_time = curTick() + pkt->headerDelay + pkt->payloadDelay +
             writeLatency;
    else {
        if (queue_delay > readLatency)
            response_time = curTick() + frontendLatency + pkt->headerDelay +
                pkt->payloadDelay;
        else
            response_time = curTick() + frontendLatency + pkt->headerDelay +
                pkt->payloadDelay + readLatency - queue_delay;
    }
    // Here we reset the timing of the packet before sending it out.
    pkt->headerDelay = pkt->payloadDelay = 0;

    DPRINTF(PMEM, "Response scheduled for %lu \n", response_time);
    port.schedTimingResp(pkt, response_time);
    DPRINTF(PMEM, "Done\n");
    return;
}

BusState
PMEMCtrl::selectNextReq() {
    BusState next = NONE;

    if (memSchedPolicy == Enums::writeHigh) {
        // Only schedule write if readQueue is empty or writeQueue is
        // above high threshold
//        if ((readQueue.empty() && !writeQueue.empty())
//         || writeQueue.size() >= writeHighThreshold) {
        if ((readQueue.empty() && !writeQueue.empty())
                || writeQueue.size() >= writeHighThreshold
                || (drainState() == DrainState::Draining && !writeQueue.empty())) {
            // In the case there is no read request to go next
            // or writeQueue is getting filled up,
            // trigger writes
            next = WRITE;
        } else if (!readQueue.empty()) {
            next = READ;
        }
    } else if (memSchedPolicy == Enums::writeHighLow) {
         // Only schedule writes if writeQueue is above high threshold
         // Keep writing till writeQueue is below low threshold
        if (busState == WRITE && writeQueue.size() >= writeLowThreshold) {
            next = WRITE;
        }
        else if (writeQueue.size() >= writeHighThreshold
                || drainState() == DrainState::Draining) {
            next = WRITE;
        } else if (!readQueue.empty()) {
            next = READ;
        }
    }
    return next;
}

void
PMEMCtrl::processNextReqEvent()
{
    PMEMPacket* pmem_pkt;

    if (busState == WRITE) {
        DPRINTF(PMEM, "Processing Write now\n");
        assert(!writeQueue.empty());
        pmem_pkt = writeQueue.front();
    } else if (busState == READ) {
        DPRINTF(PMEM, "Processing Read now\n");
        assert(!readQueue.empty());
        pmem_pkt = readQueue.front();
    } else {
        return;
    }

    DPRINTF(PMEM, "Processing Packet:%s\n", pmem_pkt->pkt->print());

    bool needsResponse = pmem_pkt->pkt->needsResponse()
                        || pmem_pkt->pkt->isResponse();
    if (pmem_pkt->burstHelper)
    {
        // it is a split packet
        pmem_pkt->burstHelper->burstsServiced++;
        if (pmem_pkt->burstHelper->burstsServiced ==
            pmem_pkt->burstHelper->burstCount) {
            // we have now serviced all children packets of a system packet
            // so we can now respond to the requester
            if (pmem_pkt->isRead) {
                access(pmem_pkt->pkt);
            }
            if (pmem_pkt->isRead || !hasPWQ) {
                if (needsResponse)
                    respond(pmem_pkt->pkt, curTick() - pmem_pkt->entryTime);
            }
            delete pmem_pkt->burstHelper;
            pmem_pkt->burstHelper = NULL;
        }
    } else {
        // it is not a split packet
        if (pmem_pkt->isRead)
        {
            access(pmem_pkt->pkt);
        }
        if (pmem_pkt->isRead || !hasPWQ) {
            if (needsResponse)
                respond(pmem_pkt->pkt, curTick() - pmem_pkt->entryTime);
        }
    }

    // Update stats
    totMemAccLat += curTick() - pmem_pkt->entryTime;
    if (pmem_pkt->isRead) {
        // Swapnil believes there is no need to consider headerDelay
        // and payloadDelay as it is simply added to the response time
        // in this stage but is not an artifact of this stage.
        bytesReadPMEM += burstLength;
        totRdLat += curTick() - pmem_pkt->entryTime;
        totRdQLat += curTick() - pmem_pkt->entryTime;
    } else {
        bytesWritten += burstLength;
        totWrLat += curTick() - pmem_pkt->entryTime;
        totWrQLat += curTick() - pmem_pkt->entryTime;
    }

    if (!pmem_pkt->isRead) {
        isInWriteQueue.erase(burstAlign(pmem_pkt->addr));
        DPRINTF(PMEM, "isInWriteQueuesize:%d\n", isInWriteQueue.size());
    }
    /* Clean out the queue */
    if (busState == WRITE) {
        writeQueue.pop_front();
        if (hasPWQ) { // We created a copy of the orignal packet
            delete pmem_pkt->pkt;
        }
    } else if (busState == READ) {
        readQueue.pop_front();
    } else {
        // Can't service a request with busState == NONE
        assert(0);
    }

    delete pmem_pkt;

    // TODO: Should we still check for retry
    // check if we are drained
    if (drainState() == DrainState::Draining &&
            readQueue.empty() && writeQueue.empty()) {
        DPRINTF(Drain, "PMEM controller done draining\n");
        signalDrainDone();
        // nothing to do, not even any point in scheduling an
        // event for the next request
        return;
    }

    /* Schedule next request */
    busState = selectNextReq();

    if (busState == READ) {
        DPRINTF(PMEM, "Scheduling read request next\n");
        schedule(nextReqEvent, curTick() + readProcessingLatency);
    }
    else if (busState == WRITE) {
        DPRINTF(PMEM, "Scheduling write request next\n");
        scheduleWrite();
    } else {
        DPRINTF(PMEM, "No requests scheduled\n");
    }

    // If there is space available and we have request waiting then let
    // them retry. This is done here to ensure that the retry does not
    // cause a nextReqEvent to be scheduled before we do so as part of
    // the next request processing
    if (retryRdReq && readQueue.size() < readBufferSize) {
        retryRdReq = false;
        port.sendRetryReq();
    }
    if (retryWrReq && writeQueue.size() < writeBufferSize) {
        retryWrReq = false;
        port.sendRetryReq();
    }
}

Port &
PMEMCtrl::getPort(const std::string &if_name, PortID idx)
{
    if (if_name != "port") {
        return ClockedObject::getPort(if_name, idx);
    } else {
        return port;
    }
}

DrainState
PMEMCtrl::drain()
{
    // if there is anything in any of our internal queues, keep track
    // of that as well
    if (!(writeQueue.empty() && readQueue.empty() && readRespQueue.empty() &&
          writeRespQueue.empty())) {

        DPRINTF(Drain, "PMEM controller not drained, write: %d, read: %d,"
                "writeResp: %d readResp: %d\n", writeQueue.size(),
                readQueue.size(), writeRespQueue.size(), readRespQueue.size());

        // the only queue that is not drained automatically over time
        // is the write queue, thus kick things into action if needed
        if (!writeQueue.empty() && !nextReqEvent.scheduled()) {
            DPRINTF(PMEM, "Beginning to drain write queue\n");
            busState = WRITE;
            scheduleWrite();
        }
        return DrainState::Draining;
    } else {
        return DrainState::Drained;
    }
}

void
PMEMCtrl::regStats()
{
    using namespace Stats;

    AbstractMemory::regStats();

    readReqs
        .name(name() + ".readReqs")
        .desc("Number of read requests accepted");

    writeReqs
        .name(name() + ".writeReqs")
        .desc("Number of write requests accepted");

    nonElidedWriteReqs
        .name(name() + ".nonElidedWriteReqs")
        .desc("Number of write requests actually performed with latency");

    readBursts
        .name(name() + ".readBursts")
        .desc("Number of PMEM read bursts, "
              "including those serviced by the write queue");

    writeBursts
        .name(name() + ".writeBursts")
        .desc("Number of PMEM write bursts, "
              "including those merged in the write queue");

    servicedByWrQ
        .name(name() + ".servicedByWrQ")
        .desc("Number of PMEM read bursts serviced by the write queue");

    mergedWrBursts
        .name(name() + ".mergedWrBursts")
        .desc("Number of PMEM write bursts merged with an existing one");

    neitherReadNorWrite
        .name(name() + ".neitherReadNorWriteReqs")
        .desc("Number of requests that are neither read nor write");

    avgRdQLen
        .name(name() + ".avgRdQLen")
        .desc("Average read queue length when enqueuing")
        .precision(2);

    avgWrQLen
        .name(name() + ".avgWrQLen")
        .desc("Average write queue length when enqueuing")
        .precision(2);

    totRdQLat
        .name(name() + ".totRdQLat")
        .desc("Total ticks spent by queued reads");

    totWrQLat
        .name(name() + ".totWrQLat")
        .desc("Total ticks spent by queued writes");

    totBusLat
        .name(name() + ".totBusLat")
        .desc("Total ticks spent in databus transfers");

    totMemAccLat
        .name(name() + ".totMemAccLat")
        .desc("Total ticks spent from burst creation until serviced "
              "by the PMEM");

    totRdLat
        .name(name() + ".totRdLat")
        .desc("Total ticks spent from burst creation until read serviced "
              "by the PMEM");

    totWrLat
        .name(name() + ".totWrLat")
        .desc("Total ticks spent from burst creation until write serviced "
              "by the PMEM");

    avgRdQLat
        .name(name() + ".avgRdQLat")
        .desc("Average queueing delay per read PMEM burst")
        .precision(2);

    avgRdQLat = totRdQLat / (readBursts - servicedByWrQ);

    avgWrQLat
        .name(name() + ".avgWrQLat")
        .desc("Average queueing delay per write PMEM burst")
        .precision(2);

    avgWrQLat = totWrQLat / writeBursts;

    avgBusLat
        .name(name() + ".avgBusLat")
        .desc("Average bus latency per PMEM burst")
        .precision(2);

    avgBusLat = totBusLat / (readBursts - servicedByWrQ);

    avgRdLat
        .name(name() + ".avgRdLat")
        .desc("Average read latency per PMEM burst")
        .precision(2);

    avgRdLat = totRdLat / (readBursts - servicedByWrQ);

    avgWrLat
        .name(name() + ".avgWrLat")
        .desc("Average write latency per PMEM burst")
        .precision(2);

    avgWrLat = totWrLat / (writeBursts);

    numRdRetry
        .name(name() + ".numRdRetry")
        .desc("Number of times read queue was full causing retry");

    numWrRetry
        .name(name() + ".numWrRetry")
        .desc("Number of times write queue was full causing retry");

    readPktSize
        .init(ceilLog2(burstLength) + 1)
        .name(name() + ".readPktSize")
        .desc("Read request sizes (log2)");

     writePktSize
        .init(ceilLog2(burstLength) + 1)
        .name(name() + ".writePktSize")
        .desc("Write request sizes (log2)");

     /*rdQLenPdf
        .init(readBufferSize)
        .name(name() + ".rdQLenPdf")
        .desc("What read queue length does an incoming req see");

     wrQLenPdf
        .init(writeBufferSize)
        .name(name() + ".wrQLenPdf")
        .desc("What write queue length does an incoming req see");*/

     rdPerTurnAround
         .init(readBufferSize)
         .name(name() + ".rdPerTurnAround")
         .desc("Reads before turning the bus around for writes")
         .flags(nozero);

     wrPerTurnAround
         .init(writeBufferSize)
         .name(name() + ".wrPerTurnAround")
         .desc("Writes before turning the bus around for reads")
         .flags(nozero);

    bytesReadPMEM
        .name(name() + ".bytesReadPMEM")
        .desc("Total number of bytes read from PMEM");

    bytesReadWrQ
        .name(name() + ".bytesReadWrQ")
        .desc("Total number of bytes read from write queue");

    bytesWritten
        .name(name() + ".bytesWritten")
        .desc("Total number of bytes written to PMEM");

    bytesReadSys
        .name(name() + ".bytesReadSys")
        .desc("Total read bytes from the system interface side");

    bytesWrittenSys
        .name(name() + ".bytesWrittenSys")
        .desc("Total written bytes from the system interface side");

    avgRdBW
        .name(name() + ".avgRdBW")
        .desc("Average PMEM read bandwidth in MegaByte/s")
        .precision(2);

    avgRdBW = (bytesReadPMEM / 1048576) / simSeconds;

    avgWrBW
        .name(name() + ".avgWrBW")
        .desc("Average achieved write bandwidth in MegaByte/s")
        .precision(2);

    avgWrBW = (bytesWritten / 1048576) / simSeconds;

    avgRdBWSys
        .name(name() + ".avgRdBWSys")
        .desc("Average system read bandwidth in MegaByte/s")
        .precision(2);

    avgRdBWSys = (bytesReadSys / 1048576) / simSeconds;

    avgWrBWSys
        .name(name() + ".avgWrBWSys")
        .desc("Average system write bandwidth in MegaByte/s")
        .precision(2);

    avgWrBWSys = (bytesWrittenSys / 1048576) / simSeconds;

    peakWriteBW
        .name(name() + ".peakWriteBW")
        .desc("Theoretical peak write bandwidth in MegaByte/s")
        .precision(2);

    peakReadBW
        .name(name() + ".peakReadBW")
        .desc("Theoretical peak read bandwidth in MegaByte/s")
        .precision(2);


    /* In gem5, bandwidth is stored in ticks/byte. 1000 ticks = 1ns.
     * Thus x gem-bw = x ticks / byte
     * => 1/x bytes/tick = 1/x  * 2^(-20) MB/tick = 1/x * (10^12)/(2^20) MB/s
     * => 1/x * 953674.31 MB/s
     */
    peakWriteBW = 953674.31/writeBandwidth;

    peakReadBW = 953674.31/readBandwidth;

    busUtil
        .name(name() + ".busUtil")
        .desc("Data bus utilization in percentage")
        .precision(2);
    // TODO
    busUtil = (avgRdBW + avgWrBW) / ((peakReadBW + peakWriteBW)/2) * 100;

    busUtilRead
        .name(name() + ".busUtilRead")
        .desc("Data bus utilization in percentage for reads")
        .precision(2);

    busUtilRead = avgRdBW / peakReadBW * 100;

    busUtilWrite
        .name(name() + ".busUtilWrite")
        .desc("Data bus utilization in percentage for writes")
        .precision(2);

    busUtilWrite = avgWrBW / peakWriteBW * 100;

    totSpecWrites
        .name(name() + ".totalSpecWrites")
        .desc("Total number of incoming speculative writes");

    totUndo
        .name(name() + ".totalUndo")
        .desc("Total number of undo records created");

    totRedo
        .name(name() + ".totalRedo")
        .desc("Total number of redo records created");

    totsafeWrAlias
        .name(name() + ".totsafeWrAlias")
        .desc("Number of safe writes aliased in URTable");

    totnumCoalesced
        .name(name() + ".totnumCoalesced")
        .desc("Number of spec writes coalesced in URTable");

    totRedoUndoAlias
        .name(name() + ".totRedoUndoAlias")
        .desc("Number of redo records written onto undo record");

    urtOccupancy
        .init(URTCapacity+1)
        .name(name() + ".urtOccupancy")
        .desc("Number of records in the URTable");
}


PMEMCtrl::MemoryPort::MemoryPort(const std::string& _name,
                                     PMEMCtrl& _memory)
    : QueuedSlavePort(_name, &_memory, queue), queue(_memory, *this),
      memory(_memory)
{ }

AddrRangeList
PMEMCtrl::MemoryPort::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(memory.getAddrRange());
    return ranges;
}

Tick
PMEMCtrl::MemoryPort::recvAtomic(PacketPtr pkt)
{
    return memory.recvAtomic(pkt);
}

void
PMEMCtrl::MemoryPort::recvFunctional(PacketPtr pkt)
{
    memory.recvFunctional(pkt);
}

bool
PMEMCtrl::MemoryPort::recvTimingReq(PacketPtr pkt)
{
    return memory.recvTimingReq(pkt);
}

PMEMCtrl*
PMEMCtrlParams::create()
{
    return new PMEMCtrl(this);
}
