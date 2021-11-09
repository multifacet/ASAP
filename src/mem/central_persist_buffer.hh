
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
        ClockedObject(params), system(params->system),
        numThreads(params->numThreads), numMCs(params->numMCs), masterId(0),
        pbCapacity(params->pbCapacity), etCapacity(params->etCapacity),
        flushThreshold(params->flushThreshold),
        flushInterval(params->flushInterval),
        flushingStarted(false)
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
            if (respQueue.isWaitingOnRetry()) {
                respQueue.retry();
            } else {
                pb.recvRespRetry(id);
            }
        }
    };

    /* Master Ports connected to L1D caches and memory */
    std::vector<PBMasterPort*> masterPorts;

    /* Slave Ports connected to cores and LLC caches */
    std::vector<PBSlavePort*> slavePorts;

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
    }

    void recvRespRetry(PortID idx)
    { masterPorts[idx]->sendRetryResp(); }

    void recvRangeChange(PortID idx)
    { slavePorts[idx]->sendRangeChange(); }

};

#endif
