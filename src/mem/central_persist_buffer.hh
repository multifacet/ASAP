
#ifndef __MEM_CENTRAL_PERSIST_BUFFER_HH__
#define __MEM_CENTRAL_PERSIST_BUFFER_HH__

#include <sstream>
#include <unordered_set>

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

    /** Flushing threshold for each per-thread PB */
    int flushThreshold;

    /** Interval between periodic flushes */
    int flushInterval;

    /** Flushing starts only after first recvTimingReq */
    bool flushingStarted;

    /** Interval between reading globalTS **/
    int pollLatency;

    /** Memory bandwidth saturated */
    std::vector<bool> memSaturated;

    /** Map to track dependencies on Acq/Rel **/
    std::map<Addr, depPair> lock_map;

    /* Keeps track of currently buffered addresses,
     * TODO FIXME Need one per PB */
    /* Multiset used as multiple copies of addresses allowed */
    std::unordered_multiset<Addr> bufferedAddr;

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
        useARP(false), numThreads(params->numThreads), numMCs(params->numMCs),
        masterId(0), pbCapacity(params->pbCapacity),
        flushThreshold(params->flushThreshold), flushInterval(params->flushInterval),
        flushingStarted(false), pollLatency(params->pollLatency), regFlushEvent(*this)
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
            PersistBuffer* pb = new PersistBuffer(*this, i);
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

    ~CentralPersistBuffer()
    {
        if (globalTS)
        free(globalTS);
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

    /* Used for printing out packet data */
    inline std::string printData(uint8_t *data, int size) {
        std::stringstream dataString;
        for (int i = 0; i < size; i++){
            dataString << std::setw(2) << std::setfill('0')
                << std::hex << "0x" << (int)data[i] << " ";
            dataString << std::setfill(' ');
        }
        dataString << std::dec;
        return dataString.str();
    }

    /* Used for printing out packetTS */
    inline std::string printTS(timestamp* ts) {
        std::stringstream tsString;
        for (int i = 0; i < numThreads; i++){
            tsString << ts[i] << ' ';
        }
        return tsString.str();
    }


    // State that is stored in packets sent to the memory controller.
    struct SenderState : public Packet::SenderState
    {
        // Id of the PB from which the flush request originated.
        PortID id;

        SenderState(PortID _id) : id(_id)
        {}
    };

  private:
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
        bool isFlushing;

        public:
        /* Vector timestamp associated with the update */
        timestamp* storeVecTS;

        PBEntry()
        {
        }

        PBEntry(PacketPtr pkt, timestamp* ts, PortID id, int numThreads)
        {
            storeTS = ts[id];
            // Aligning the address
            addr = pkt->getAddr() - pkt->getOffset(BLK_SIZE);
            // Taken care of by setMask() but need to initialize
            size = 0;
            isFlushing = false;
            writeMask = std::vector<bool>(BLK_SIZE, false);
            setMask(pkt->getOffset(BLK_SIZE), pkt->getSize());
            dataPtr = new uint8_t[BLK_SIZE];
            // Writes at correct Offset
            pkt->writeDataToBlock(dataPtr, BLK_SIZE);

            storeVecTS = (timestamp *) malloc(numThreads * sizeof(timestamp));
            //Vector Timestamp
            for (int i = 0; i < numThreads; ++i) {
                storeVecTS[i] = ts[i];
            }
        }

        ~PBEntry()
        {
            /* Default constructor does not allocate data */
            if (dataPtr)
                delete[] dataPtr;
            if (storeVecTS)
                free(storeVecTS);
        }

        timestamp getTS(){
            return storeTS;
        }

        timestamp* getVecTS(){
            return storeVecTS;
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

        std::string printData() const
        {
            std::stringstream dataStr;
            for (int i = 0; i < BLK_SIZE; i++) {
                if (writeMask[i]) {
                    dataStr << std::setw(2) << std::setfill('0') << std::hex
                            << "0x" << (int)dataPtr[i] << " ";
                    dataStr << std::setfill(' ');
                }
                else {
                    dataStr << " - ";
                }
            }
            dataStr << std::dec ;
            return dataStr.str();
        }

        bool getIsFlushing() {
            return isFlushing;
        }

        void setIsFlushing() {
            isFlushing = true;
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

        /* Using PortID to identify owner (core) */
        PortID id;

        /* Number of writes coalesced in latest epoch */
        int numCoalesced;

        /* Number of pbEntries in latest epoch */
        int epochEntries;

        public:

        bool dFenceInProg;

        /* Set if a request was rejected due to a full buffer */
        bool retryWrReq;

        /* List of all dFence packets currently being served */
        std::deque<PacketPtr> dfencePkts;

        /* Flush PB! */
        void processFlushEvent();
        EventWrapper<PersistBuffer, &PersistBuffer::processFlushEvent>
        flushEvent;

        void pollingGlobalTS();
        EventWrapper<PersistBuffer, &PersistBuffer::pollingGlobalTS>
        pollGlobalTSEvent;

        /* Statistics to be tracked per buffer*/

        /* Size of epoch */
        Stats::Histogram epochSize;

        /* Number of entries per PB */
        Stats::Histogram pbOccupancy;

        /* WAW reuse within epochs */
        Stats::Histogram wawHits;

        /* Number of stalled cycles due to dfence */
        Stats::Scalar dfenceCycles;

        /* Number of stalled cycles due to full PB */
        Stats::Scalar stallCycles;

        /* Number of stalled cycles without flushing */
        Stats::Scalar noflushCyclesIntra;
        Stats::Scalar noflushCyclesInter;

        /* Current vector timestamp? */
        timestamp* currVecTS;

        /* Copy of globalTS read periodically */
        timestamp* readGlobalTS;

        /* Start time of current stall */
        Tick stallStart;

        /* Start time of current dfence */
        Tick dfenceStart;

        PersistBuffer(CentralPersistBuffer& pb, PortID id) :
            EventManager(&pb), unflushedEntries(0), pb(pb), id(id),
            numCoalesced(0), epochEntries(0), dFenceInProg(false),
            retryWrReq(false), flushEvent(*this), pollGlobalTSEvent(*this),
            stallStart(0)
        {
            currVecTS = (timestamp *) malloc(pb.numThreads*sizeof(timestamp));
            //Vector Timestamp
            for (int i = 0; i < pb.numThreads; ++i) {
                currVecTS[i] = 0;
            }
            currVecTS[id] = 1;

            readGlobalTS = (timestamp *) malloc(pb.numThreads*sizeof(timestamp));
            for (int i = 0; i < pb.numThreads; ++i) {
                readGlobalTS[i] = -1;
            }
            readGlobalTS[id] = 0;
        }

        ~PersistBuffer()
        {
            if (currVecTS)
                free(currVecTS);
            if (readGlobalTS)
                free(readGlobalTS);
        }

        virtual const std::string name() const {
            return csprintf("PersistBuffer%d", id);
        }

        void regStats();

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
                if (!entry->getIsFlushing())
                    return entry;
            }
            return NULL;

        }

        PortID getThreadID(){
            return id;
        }

        timestamp getCurrentTS() {
            return currVecTS[id];
        }

        bool isStalled(){
            return retryWrReq;
        }

        void stallPB(){
            setRetryReq();
            stallStart = curTick();
        }

        void setRetryReq(){
            retryWrReq = true;
        }

        /* Figure out if flushing is allowed for this epoch */
        inline bool flushOkay(timestamp* TS) {
            for (int i=0; i<pb.numThreads; i++) {
                /* All other positions of TS have to be <= globalTS */
                if (TS[i]>readGlobalTS[i] && i!=id)
                    return false;
            }
            return true;
        }

        void serviceOFence(){
            if (PBEntries.size() == 0)
                pb.globalTS[id] = currVecTS[id];

            currVecTS[id]++;

            DPRINTF(PersistBuffer, "Core%d OFence TS=%d\n", id, getCurrentTS());
            /* Update and clear all stats at the end of the epoch! */
            if (epochEntries > pb.pbCapacity)
                epochEntries = pb.pbCapacity;
            epochSize.sample(epochEntries);
            wawHits.sample(numCoalesced);
            numCoalesced = 0;
            epochEntries = 0;
        }

        void serviceDFence(){
            dFenceInProg = true;
            dfenceStart = curTick();

            if (!flushEvent.scheduled())
                schedule(flushEvent, pb.nextCycle());
        }

        void respondToDFence()
        {
            assert(dfenceStart > 0);
            dfenceCycles += (curTick() - dfenceStart);
            dfenceStart = 0;
            dFenceInProg = false;
            serviceOFence();
            while (dfencePkts.size()>0) {
                DPRINTF(PersistBuffer, "PB%d DFence completed! Epoch=%d\n", id, currVecTS[id]-1);
                PacketPtr respPkt = dfencePkts.front();
                respPkt->makeResponse();
                DPRINTF(PersistBufferDebug, "respondToDFence id:%d Addr:0x%x"
                        "isPMFence:%d\n", id, respPkt->getAddr(),
                        respPkt->req->isPMFence());
                if (!pb.recvTimingResp(respPkt, id))
                    assert(0); // We don't have code to handle failure
                dfencePkts.pop_front();
            }
        }

        void addCrossThreadDep(PortID srcCore, timestamp TS)
        {
            currVecTS[srcCore] = TS;
        }

        void flushPB();

        void flushAck(PacketPtr pkt);
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
            if (!regFlushEvent.scheduled())
                schedule(regFlushEvent, clockEdge(Cycles(flushInterval)));
        }
    }

    void recvRespRetry(PortID idx)
    { masterPorts[idx]->sendRetryResp(); }

    void recvRangeChange(PortID idx)
    { slavePorts[idx]->sendRangeChange(); }

};

#endif
