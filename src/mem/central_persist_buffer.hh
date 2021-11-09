
#ifndef __MEM_CENTRAL_PERSIST_BUFFER_HH__
#define __MEM_CENTRAL_PERSIST_BUFFER_HH__

#include <sstream>
#include <unordered_set>
#include <utility>

#include "base/statistics.hh"
#include "debug/PersistBuffer.hh"
#include "debug/PersistBufferDebug.hh"
#include "debug/ProtocolTrace.hh"
#include "mem/mem_object.hh"
#include "mem/packet.hh"
#include "mem/ruby/system/RubyPort.hh"
#include "params/CentralPersistBuffer.hh"
#include "sim/eventq.hh"
#include "sim/system.hh"

#define BLK_SIZE 64

#define _max(X, Y)  ((X) > (Y) ? (X) : (Y))

#define depPair std::pair<PortID, uint64_t>

enum writeType {
    SAFE = 0,
    SPECULATIVE,
    RETRY,
    INVALID
};

enum flushStatus {
    WAITING = 0,
    FLUSHED,
    NACKED
};

class CentralPersistBuffer : public ClockedObject
{

  public:

    /** Parameters of Central Persist Buffer */
    typedef CentralPersistBufferParams Params;

    /* System pointer for isMemAddr() */
    System* system;

    std::string pModel;
    bool useARP;

    /* Timestamp type */
    typedef uint64_t timestamp;

    /** Number of HW threads being monitored by PB */
    int numThreads;

    /** Number of memory controllers being monitored by PB */
    int numMCs;

    /** MasterID used by some components of gem5. */
    MasterID masterId; //TODO FIXME, use getMasterID

    /** Maximum size of a per-thread PB */
    int pbCapacity;

    /** Maximum size of a per-thread Epoch Table */
    int etCapacity;

    /** Flushing threshold for each per-thread PB */
    int flushThreshold;

    /** Interval between periodic flushes */
    int flushInterval;

    /** Flushing starts only after first recvTimingReq */
    bool flushingStarted;

    /** Memory bandwidth saturated */
    std::vector<bool> memSaturated;

    /* Keeps track of currently buffered addresses,
     * TODO FIXME Need one per PB */
    /* Multiset used as multiple copies of addresses allowed */
    std::unordered_multiset<Addr> bufferedAddr;

    std::map<Addr, depPair> lock_map;
    std::map<Addr, PortID> simpleDir;

    /* Flush all PBs! */
    void processRegFlushEvent();
    EventWrapper<CentralPersistBuffer,
        &CentralPersistBuffer::processRegFlushEvent> regFlushEvent;

    /* Miss from the LLC which hits in the PB */
    Stats::Scalar missConflict;

    /* R/W from other threads which hit in the PB */
    Stats::Scalar interTEpochConflict;

    /* Number of entries inserted into all PBs */
    Stats::Scalar entriesInserted;

    /* Number of entries flushed from all PBs */
    Stats::Scalar entriesFlushed;

    /* Number of OFENCEs encountered across all PBs */
    Stats::Scalar ofenceTotal;

    /* Number of DFENCEs encountered across all PBs */
    Stats::Scalar dfenceTotal;

    /* Number of accesses to PM */
    Stats::Scalar pmAccesses;

    /* Number of accesses to DRAM */
    Stats::Scalar dramAccesses;

    /* Cycles stalled because memory bandwidth saturated */
    Stats::Scalar bwSatCycles;
    std::vector<Tick> bwSatStart;

    /**
     * Constructor based on the Python params
     *
     * @param params Python parameters
     */
    CentralPersistBuffer(Params* params) :
        ClockedObject(params), system(params->system), pModel(params->pModel),
        useARP(true), numThreads(params->numThreads), numMCs(params->numMCs),
        masterId(0), pbCapacity(params->pbCapacity), etCapacity(params->etCapacity),
        flushThreshold(params->flushThreshold),
        flushInterval(params->flushInterval),
        flushingStarted(false), regFlushEvent(*this)
    {

        //CPU SIDE MASTER-SLAVE PORTS
        // create the ports based on the size of the master and slave
        // vector ports, and the presence of the default port, the ports
        // are enumerated starting from zero
        for (int i = 0; i < numThreads; ++i) {
            std::string portName = csprintf("%s.cpuMaster[%d]", name(), i);
            PBMasterPort* bp = new PBMasterPort(portName, *this, i);
            masterPorts.push_back(bp);
            DPRINTF(PersistBuffer, "Instantiated PBport %s\n", bp->name());
        }

        for (int i = 0; i < numThreads; ++i) {
        // create the slave ports, once again starting at zero
            std::string portName = csprintf("%s.cpuSlave[%d]", name(), i);
            PBSlavePort* bp = new PBSlavePort(portName, *this, i);
            slavePorts.push_back(bp);
            DPRINTF(PersistBuffer, "Instantiated PBport %s\n", bp->name());
        }

        //MEMORY SIDE MASTER-SLAVE PORTS
        // BOTH TYPES OF PORTS ARE in one Vector Port internally!!!!
        for (int i = 0; i < numMCs; ++i) {
            std::string portName = csprintf("%s.memMaster[%d]", name(), i);
            PBMasterPort* bp = new PBMasterPort(portName, *this, numThreads+i);
            masterPorts.push_back(bp);
            DPRINTF(PersistBuffer, "Instantiated PBport %s\n", bp->name());
        }

        for (int i = 0; i < numMCs; ++i) {
            std::string portName = csprintf("%s.memSlave[%d]", name(), i);
            PBSlavePort* bp = new PBSlavePort(portName, *this, numThreads+i);
            slavePorts.push_back(bp);
            DPRINTF(PersistBuffer, "Instantiated PBport %s\n", bp->name());
        }

        // PERSIST BUFFERS
        // create the per-core persist buffers, starting at zero
        for (int i = 0; i < numThreads; ++i) {
            EpochTable* et = new EpochTable(*this, i);
            perThreadETs.push_back(et);
            PersistBuffer* pb = new PersistBuffer(*this, *et, i);
            perThreadPBs.push_back(pb);
            DPRINTF(PersistBuffer, "Instantiated per-thread PBs %s\n",
                    pb->name());
        }

        globalTS = (timestamp *) malloc(numThreads * sizeof(timestamp));
        //Vector Timestamp
        for (int i = 0; i < numThreads; ++i) {
            globalTS[i] = 0;
        }

        memSaturated = std::vector<bool>(numMCs, false);
        bwSatStart = std::vector<Tick>(numThreads, 0);
    }

    Port& getPort(const std::string& if_name,
                                  PortID idx)
    {
        if (if_name == "cpuMaster" && idx < numThreads) {
            return *masterPorts[idx];
        } else if (if_name == "memMaster" && idx < numMCs) {
            return *masterPorts[numThreads+idx];
        } else if (if_name == "cpuSlave" && idx < numThreads) {
            return *slavePorts[idx];
        } else if (if_name == "memSlave" && idx < numMCs) {
            return *slavePorts[idx+numThreads];
        } else {
            return ClockedObject::getPort(if_name, idx);
        }
    }

    void regStats();

    inline Addr alignAddr(Addr a)
        { return (a & ~(Addr(BLK_SIZE - 1))); }

    // State that is stored in packets sent to the memory controller.
    struct SenderInfo : public Packet::SenderState
    {
        // Id of the PB from which the flush request originated.
        PortID coreID;

        timestamp TS;

        writeType type;

        SenderInfo(PortID _id)
            : coreID(_id), TS(0), type(INVALID)
        {}
        SenderInfo(PortID _id, timestamp _TS, writeType _type)
            : coreID(_id), TS(_TS), type(_type)
        {}
    };

    void handlePMWriteback(PacketPtr pkt);

  private:
    class ETEntry
    {
        public:
        timestamp TS;

        // ACK count
        int pendingACKs;

        // Dependence <coreID, epoch>
        depPair crossThreadDep;

        // MC bit vector
        std::vector<bool> mcSpecMask;
        std::vector<bool> mcWriteMask;

        // Dependent threads
        PortID dependentThread;

        bool isFlushing;

        ETEntry(timestamp TS, int& numMCs)
            : TS(TS), pendingACKs(0), dependentThread(-1),
            isFlushing(false)
        {
            crossThreadDep = std::make_pair(-1,-1);
            mcSpecMask = std::vector<bool>(numMCs, false);
            mcWriteMask = std::vector<bool>(numMCs, false);
        }

        timestamp getTS()
        {
            return TS;
        }

        void incPendingACKs()
        {
            pendingACKs++;
        }

        void decPendingACKs()
        {
            pendingACKs--;
            assert(pendingACKs >= 0);
        }

        int getPendingACKs()
        {
            return pendingACKs;
        }

        void setMcSpecMask(int mcOffset)
        {
            assert(mcOffset < mcSpecMask.size());
            mcSpecMask[mcOffset] = true;
        }

        void clearMcSpecMask(int mcOffset)
        {
            assert(mcOffset < mcSpecMask.size());
            mcSpecMask[mcOffset] = false;
        }

        void setMcWriteMask(int mcOffset)
        {
            assert(mcOffset < mcWriteMask.size());
            mcWriteMask[mcOffset] = true;
        }

        void clearMcWriteMask(int mcOffset)
        {
            assert(mcOffset < mcWriteMask.size());
            mcWriteMask[mcOffset] = false;
        }
        bool getHasSpecWrites()
        {
            for (auto mc: mcSpecMask) {
                if (mc)
                    return mc;
            }
            return false;
        }

        void setIsFlushing(bool val)
        {
            isFlushing = val;
        }

        bool getIsFlushing()
        {
            return isFlushing;
        }
    };

    class PBEntry
    {
        /* A pointer to the data being stored. */
        uint8_t* dataPtr;

        /* The address of the request.  This address could be virtual or
        * physical, depending on the system configuration. */
        Addr addr;

        /* Timestamp associated with the update */
        timestamp storeTS;

        /// The size of the request or transfer.
        unsigned size;

        // Write mask, used for coalescing
        std::vector<bool> writeMask;

        // Indicates if flushing has begun for this entry ..
        flushStatus fStatus;

        public:

        PBEntry(PacketPtr pkt, timestamp ts)
            : storeTS(ts), size(0), fStatus(WAITING)
        {
            // Aligning the address
            addr = pkt->getAddr() - pkt->getOffset(BLK_SIZE);
            // Taken care of by setMask() but need to initialize
            writeMask = std::vector<bool>(BLK_SIZE, false);
            setMask(pkt->getOffset(BLK_SIZE), pkt->getSize());
            dataPtr = new uint8_t[BLK_SIZE];
            // Writes at correct Offset
            pkt->writeDataToBlock(dataPtr, BLK_SIZE);
        }

        ~PBEntry()
        {
            /* Default constructor doesn not allcoate data */
            if (dataPtr)
                delete[] dataPtr;
        }

        timestamp getTS(){
            return storeTS;
        }

        Addr getAddr(){
            return addr;
        }

        int getSize(){
            return size;
        }

        std::vector<bool> getMask()
        {
            return writeMask;
        }

        std::string printMask() const
        {
            std::string str(BLK_SIZE,'0');
            for (int i = 0; i < BLK_SIZE; i++) {
                str[i] = writeMask[i] ? ('1') : ('0');
            }
            return str;
        }

        void setMask(int offset, int len)
        {
            assert(BLK_SIZE >= (offset + len));
            for (int i = 0; i < len; i++) {
                if (!writeMask[offset + i]) {
                    writeMask[offset + i] = true;
                    size++;
                }
            }
        }

        uint8_t* getDataPtr(){
            return dataPtr;
        }

        flushStatus getFlushStatus() {
            return fStatus;
        }

        void setFlushStatus(flushStatus s) {
            fStatus = s;
        }
    };

    class EpochTable : public EventManager
    {
        std::deque<ETEntry*> epochs;

        ETEntry* currentEpoch;

        CentralPersistBuffer& cpb;

        PortID coreID;

        // TODO: when to update this
        std::vector<timestamp> safeTS;

        std::deque<PacketPtr> dfencePkts;

        std::deque<depPair> ecMsgs;

        public:

        bool dfenceInProg;
        // Stats
        /* Start time of current dfence */
        Tick dfenceStart;

        /* Number of writes coalesced in latest epoch */
        int numCoalesced;

        /* Number of writes in an epoch */
        int epochEntries;

        /* Set if a packet was rejected due to a full table */
        bool retryPkt;

        /* Start time of current stall */
        Tick etStallStart;

        /* Process ET every cycle */
        // TODO: See if this can be done only after receiving an
        //       ACK or when cross thread dep resolved.
        void processETCycle();
        EventWrapper<EpochTable, &EpochTable::processETCycle>
        ETCycle;

        void processECEvent();
        EventWrapper<EpochTable, &EpochTable::processECEvent>
        ECEvent;

        virtual const std::string name() const {
            return csprintf("EpochTable%d", coreID);
        }

        EpochTable(CentralPersistBuffer& _cpb, PortID id)
            : EventManager(&_cpb), cpb(_cpb), coreID(id),
              dfenceInProg(false), dfenceStart(0), numCoalesced(0),
              epochEntries(0), retryPkt(false), etStallStart(0),
              ETCycle(*this), ECEvent(*this)
        {
            safeTS = std::vector<timestamp>(cpb.numMCs, 0);
            currentEpoch = new ETEntry(0, cpb.numMCs);
        }

        ~EpochTable()
        {
            if (currentEpoch)
                delete currentEpoch;
        }

        int getSize()
        {
            return epochs.size();
        }

        void incPendingACKs()
        {
            currentEpoch->incPendingACKs();
        }

        void decPendingACKs(timestamp TS)
        {
            if (currentEpoch->TS == TS) {
                currentEpoch->decPendingACKs();
            }
            else {
                for (auto epoch : epochs) {
                    if (epoch->TS == TS) {
                        epoch->decPendingACKs();
                        break;
                    }
                }
            }
        }

        timestamp getCurrentTS()
        {
            return currentEpoch->TS;
        }

        void setMcSpecMask(timestamp TS, int mcOffset)
        {
            if (currentEpoch->TS == TS)
                currentEpoch->setMcSpecMask(mcOffset);
            else {
                for (auto entry: epochs) {
                    if (entry->TS == TS)
                        entry->setMcSpecMask(mcOffset);
                }
            }
        }

        void clearMcSpecMask(timestamp TS, int mcOffset)
        {
            if (currentEpoch->TS == TS)
                currentEpoch->clearMcSpecMask(mcOffset);
            else {
                for (auto entry: epochs) {
                    if (entry->TS == TS)
                        entry->clearMcSpecMask(mcOffset);
                }
            }
        }

        void setMcWriteMask(timestamp TS, int mcOffset)
        {
            if (currentEpoch->TS == TS)
                currentEpoch->setMcWriteMask(mcOffset);
            else {
                for (auto entry: epochs) {
                    if (entry->TS == TS)
                        entry->setMcWriteMask(mcOffset);
                }
            }
        }
        void createNewEpoch()
        {
            // Removes noise from the first epoch
            if (epochEntries > cpb.pbCapacity)
                epochEntries = cpb.pbCapacity;
            if (numCoalesced > cpb.pbCapacity)
                numCoalesced = cpb.pbCapacity;
            cpb.perThreadPBs[coreID]->epochSize.sample(epochEntries);
            cpb.perThreadPBs[coreID]->etOccupancy.sample(getSize());
            cpb.perThreadPBs[coreID]->wawHits.sample(numCoalesced);
            epochEntries = 0;
            numCoalesced = 0;

            epochs.push_back(currentEpoch);
            currentEpoch = new ETEntry(getCurrentTS()+1, cpb.numMCs);

            if (!ETCycle.scheduled())
                schedule(ETCycle, curTick());

            DPRINTF(PersistBufferDebug, "Core%d new epoch%d\n", coreID, getCurrentTS());
        }

        void setRetryReq(){
            retryPkt = true;
            etStallStart = curTick();
        }

        bool isFlushSafe(timestamp TS, int mc)
        {
            if (TS > safeTS[mc])
                return false;

            if (TS == getCurrentTS()) {
                if (currentEpoch->crossThreadDep.first == -1)
                    return true;
                else
                    return false;
            }

            for (auto entry:epochs) {
                if (entry->TS == TS) {
                    if (entry->crossThreadDep.first == -1)
                        return true;
                    else
                        return false;
                }
            }

            return false;
        }

        bool checkMcMask(timestamp TS, int mc)
        {
            std::vector<bool> cummMask;
            cummMask = std::vector<bool>(cpb.numMCs, false);

            bool spec = false;

            for (auto entry: epochs) {
                if (entry->TS == TS)
                    break;
                for (int i=0; i<cummMask.size(); ++i)
                    cummMask[i] = entry->mcWriteMask[i]?true:false;
            }

            for (int i=0; i<cummMask.size(); ++i) {
                if (i != mc)
                    spec |= cummMask[i];
            }

            return !spec;
        }

        void serviceOFence()
        {
            createNewEpoch();
            DPRINTF(PersistBuffer, "PB%d ofence, TS=%d\n", coreID, getCurrentTS());
        }

        bool serviceDFence(PacketPtr p)
        {
            DPRINTF(PersistBuffer, "PB%d dfence\n", coreID);
            createNewEpoch();
            if (epochs.size() == 1) {
                ETEntry *first = epochs.front();
                if (first->getPendingACKs() == 0 &&
                    first->crossThreadDep.first == -1) {
                    if (first->dependentThread != -1)
                        cpb.perThreadETs[first->dependentThread]->
                            addECMsg(coreID, first->getTS());
                    delete epochs.front();
                    epochs.erase(epochs.begin());
                    return true;
                }
            }
            dfencePkts.push_back(p);
            dfenceInProg = true;
            dfenceStart = curTick();
            return false;
        }

        void handleEpochCompletion(timestamp TS, PortID mcNum);

        bool isEpochComplete(timestamp TS)
        {
            if (getCurrentTS() == TS)
                return false;
            for (auto entry: epochs) {
                if (entry->TS == TS)
                    return false;
            }
            return true;
        }

        void addCrossThreadDep(PortID srcCore, timestamp srcTS)
        {
            currentEpoch->crossThreadDep = std::make_pair(srcCore, srcTS);
        }

        timestamp addDepThread(PortID depCore)
        {
            if (epochs.size() > 0) {
                ETEntry *last = epochs.back();
                if (last->dependentThread == -1) {
                    last->dependentThread = depCore;
                    return last->getTS();
                }
                else {
                    currentEpoch->dependentThread = depCore;
                    createNewEpoch();
                    DPRINTF(PersistBuffer, "Epoch dependency collision\n");
                    return getCurrentTS() - 1;
                }
            }

            return -1;
        }

        void addECMsg(PortID srcCore, timestamp TS)
        {
            ecMsgs.push_back(std::make_pair(srcCore, TS));

            if (!ECEvent.scheduled())
                // TODO: add delay to the message
                schedule(ECEvent, cpb.nextCycle());
        }
    };

    class PersistBuffer : public EventManager
    {
        /* TODO replace this by a RAM? */
        std::deque<PBEntry*> PBEntries;

        /* Size of unflushed PB */
        int unflushedEntries;

        /* Need a pointer to this for initiating mem WBs*/
        CentralPersistBuffer& pb;

        /* Pointer to the Epoch Table */
        EpochTable& et;

        /* Using PortID to identify owner (core) */
        PortID id;

        public:

        bool dFenceInProg;

        /* Set if a request was rejected due to a full buffer */
        bool retryWrReq;

        /* List of all dFence packets currently being served */
        std::deque<PacketPtr> dfencePkts;

        /* Flush all PBs! */
        void processFlushEvent();
        EventWrapper<PersistBuffer, &PersistBuffer::processFlushEvent>
        flushEvent;

        bool stopSpecFlush;

        /* Statistics to be tracked per buffer*/

        /* Size of epoch */
        Stats::Histogram epochSize;

        /* Number of entries per PB */
        Stats::Histogram pbOccupancy;

        /* Number of entries per ET */
        Stats::Histogram etOccupancy;

        /* WAW reuse within epochs */
        Stats::Histogram wawHits;

        /* Number of stalled cycles due to dfence */
        Stats::Scalar dfenceCycles;

        /* Number of stalled cycles due to full PB */
        Stats::Scalar stallCycles;

        /* Number of stalled cycles without flushing */
        Stats::Scalar noflushCycles;

        /* Number of stalled cycles without flushing */
        Stats::Scalar etStallCycles;

        /* Number of Nack bursts */
        Stats::Scalar numNackBursts;

        /* Start time of current stall */
        Tick stallStart;

        PersistBuffer(CentralPersistBuffer& pb, EpochTable& et, PortID id) :
            EventManager(&pb), unflushedEntries(0), pb(pb), et(et), id(id),
            dFenceInProg(false), retryWrReq(false),
            flushEvent(*this), stopSpecFlush(false), stallStart(0)
        {
        }

        virtual const std::string name() const {
            return csprintf("PersistBuffer%d", id);
        }

        void regStats();


        /* returns 1 if A > B, 0 if A == B, -1 if A<B  TODO */
        /* return true if A == B */
        bool compareVecTS(timestamp* tsA, timestamp* tsB) {
            for (int i = 0; i < pb.numThreads; i++) {
                if (tsA[i] != tsB[i])
                    return false;
            }
            return true;
        }

        void checkCrossDependency(PacketPtr pkt);
        bool tryCoalescePBEntry(PacketPtr pkt);
        bool coalescePBEntry(PacketPtr pkt);
        void addPBEntry(PacketPtr pkt);

        PBEntry* getPBHead() {
            return getOldestUnflushed();
        }

        PBEntry* getPBTail() {
            return PBEntries.back();
        }

        int getSize(){
            return PBEntries.size();
        }

        PBEntry* getOldestUnflushed(){
            for (auto entry : PBEntries){
                if (entry->getFlushStatus() == WAITING ||
                     entry->getFlushStatus() == NACKED)
                    return entry;
            }
            return NULL;

        }

        PortID getThreadID(){
            return id;
        }

        bool isStalled(){
            return retryWrReq;
        }

        void stallPB(){
            retryWrReq = true;
            stallStart = curTick();
        }

        void flushPB();

        void flushAck(PacketPtr pkt);

        void handleUTFullError(PacketPtr pkt);

        bool findAddr(Addr addr)
        {
            for (auto entry: PBEntries) {
                if (entry->getAddr() == addr)
                    return true;
            }
            return false;
        }
    };

    class PBMasterPort : public MasterPort
    {
        /** A reference to the central persist buffer to which this belongs */
        CentralPersistBuffer& pb;


        public:
        PBMasterPort(const std::string& name, CentralPersistBuffer &pb,
                PortID idx) :
            MasterPort(name, &pb, idx), pb(pb)
        {
        }

        void recvFunctionalSnoop(PacketPtr pkt)
        {
            DPRINTF(PersistBufferDebug, "recvFuncSnoop: %s 0x%x\n",
                    pkt->cmdString(), pkt->getAddr());
            pb.recvFunctionalSnoop(pkt, id);
        }

        Tick recvAtomicSnoop(PacketPtr pkt)
        {
            DPRINTF(PersistBufferDebug, "recvAtomicSnoop: %s 0x%x\n",
                    pkt->cmdString(), pkt->getAddr());
            return pb.recvAtomicSnoop(pkt, id);
        }

        bool recvTimingResp(PacketPtr pkt)
        {
            DPRINTF(PersistBufferDebug, "recvTimingResp: %s 0x%x\n",
                    pkt->cmdString(), pkt->getAddr());
            return pb.recvTimingResp(pkt, id);
        }

        void recvTimingSnoopReq(PacketPtr pkt)
        {
            DPRINTF(PersistBufferDebug, "recvTimingSnoopReq: %s 0x%x\n",
                   pkt->cmdString(), pkt->getAddr());
            pb.recvTimingSnoopReq(pkt, id);
        }

        void recvRangeChange()
        { pb.recvRangeChange(id); }

        bool isSnooping() const
        { return pb.isSnooping(id); }

        void recvReqRetry()
        {
            pb.recvReqRetry(id);
        }

        void recvRetrySnoopResp()
        { pb.recvRetrySnoopResp(id); }
    };

    class PBSlavePort : public QueuedSlavePort
    {
        /** A reference to the central persist buffer to which this belongs */
        CentralPersistBuffer& pb;

        public:

        PBSlavePort(const std::string& name, CentralPersistBuffer &pb,
                PortID idx) :
                QueuedSlavePort(name, &pb, respQueue, idx), pb(pb),
                respQueue(pb, *this)
        { }

        protected:
        RespPacketQueue respQueue;

        void recvFunctional(PacketPtr pkt)
        {
            DPRINTF(PersistBufferDebug, "recvFunctional: %s 0x%x\n",
                   pkt->cmdString(), pkt->getAddr());
            pb.recvFunctional(pkt, id);
        }

        Tick recvAtomic(PacketPtr pkt)
        {
            DPRINTF(PersistBufferDebug, "recvAtomic: %s 0x%x\n",
                    pkt->cmdString(), pkt->getAddr());
            return pb.recvAtomic(pkt, id);
        }

        bool recvTimingReq(PacketPtr pkt)
        {
            DPRINTF(PersistBufferDebug, "recvTimingReq:%s 0x%x isPMFence:%d\n",
                    pkt->cmdString(), pkt->getAddr(), pkt->req->isPMFence());
            return pb.recvTimingReq(pkt, id);
        }

        bool recvTimingSnoopResp(PacketPtr pkt)
        {
            DPRINTF(PersistBufferDebug, "recvTimingSnoopResp: %s 0x%x\n",
                    pkt->cmdString(), pkt->getAddr());
            return pb.recvTimingSnoopResp(pkt, id);
        }

        AddrRangeList getAddrRanges() const
        { return pb.getAddrRanges(id); }

        void recvRespRetry()
        {
            pb.recvRespRetry(id);
            if (respQueue.isWaitingOnRetry())
                respQueue.retry();
        }
    };

    /* Master Ports connected to L1D caches and memory */
    std::vector<PBMasterPort*> masterPorts;

    /* Slave Ports connected to cores and LLC caches */
    std::vector<PBSlavePort*> slavePorts;

    /* Per-thread Persist Buffers */
    std::vector<PersistBuffer *> perThreadPBs;

    /* Per-thread Epoch Tables */
    std::vector<EpochTable *> perThreadETs;

    /* Latest global timestamp*/
    timestamp* globalTS;

    void recvFunctional(PacketPtr pkt, PortID idx)
    { masterPorts[idx]->sendFunctional(pkt); }

    void recvFunctionalSnoop(PacketPtr pkt, PortID idx)
    { slavePorts[idx]->sendFunctionalSnoop(pkt); }

    Tick recvAtomic(PacketPtr pkt, PortID idx)
    { return masterPorts[idx]->sendAtomic(pkt); }

    Tick recvAtomicSnoop(PacketPtr pkt, PortID idx)
    { return slavePorts[idx]->sendAtomicSnoop(pkt); }

    void checkAndMarkNVM(PacketPtr pkt);

    bool recvTimingReq(PacketPtr pkt, PortID idx);

    bool recvTimingResp(PacketPtr pkt, PortID idx);

    void recvTimingSnoopReq(PacketPtr pkt, PortID idx)
    { slavePorts[idx]->sendTimingSnoopReq(pkt); }

    bool recvTimingSnoopResp(PacketPtr pkt, PortID idx)
    { return masterPorts[idx]->sendTimingSnoopResp(pkt); }

    void recvRetrySnoopResp(PortID idx)
    { slavePorts[idx]->sendRetrySnoopResp(); }

    AddrRangeList getAddrRanges(PortID idx) const
    { return masterPorts[idx]->getAddrRanges(); } // TODO FIXME

    bool isSnooping(PortID idx) const
    { return slavePorts[idx]->isSnooping(); }

    void recvReqRetry(PortID idx)
    {
        slavePorts[idx]->sendRetryReq();
        if (idx >= numThreads && memSaturated[idx-numThreads]) {
            DPRINTF(PersistBuffer, "Received retry from portID:%d\n", idx);
            memSaturated[idx-numThreads] = false;
            assert (bwSatStart[idx-numThreads] > 0);
            bwSatCycles += (curTick() - bwSatStart[idx-numThreads]);
            bwSatStart[idx-numThreads] = 0;
            for (int i = 0; i < numThreads; ++i) {
                if (!perThreadPBs[i]->flushEvent.scheduled()) {
                    schedule(perThreadPBs[i]->flushEvent, nextCycle());
                }
            }
        }
    }

    void recvRespRetry(PortID idx)
    { masterPorts[idx]->sendRetryResp(); }

    void recvRangeChange(PortID idx)
    { slavePorts[idx]->sendRangeChange(); }

};

#endif
