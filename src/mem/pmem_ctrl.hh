/*
 * Authors: Swapnil Haria
 */

/**
 * @file
 * PMEMCtrl declaration
 */

#ifndef __PMEM_CTRL_HH__
#define __PMEM_CTRL_HH__

#include <deque>
#include <unordered_set>

#include "base/statistics.hh"
#include "enums/AddrMap1.hh"
#include "enums/PMemSched.hh"
#include "enums/PageManage1.hh"
#include "mem/abstract_mem.hh"
#include "mem/central_persist_buffer.hh"
#include "mem/qport.hh"
#include "params/PMEMCtrl.hh"

#define BLK_SIZE 64

/* Tracks command to be processed next
*/
enum BusState {
    READ = 0,
    WRITE,
    NONE,
};

enum URRecordType {
    UNDO = 0,
    REDO
};


/**
 * The PMEM Controller is a basic single-ported memory controller with
 * configurable throughput and latency separately for reads and writes.
 */
class PMEMCtrl: public AbstractMemory
{

  private:

    typedef uint64_t timestamp;

    typedef CentralPersistBuffer::SenderInfo SenderInfo;

    // Use of queued slave port hides flow control issues,
    // but is this the right approach?
    class MemoryPort: public QueuedSlavePort
    {

      private:
        RespPacketQueue queue;
        PMEMCtrl& memory;

      public:

        MemoryPort(const std::string& _name, PMEMCtrl& _memory);

      protected:

        Tick recvAtomic(PacketPtr pkt);

        void recvFunctional(PacketPtr pkt);

        bool recvTimingReq(PacketPtr pkt);

        virtual AddrRangeList getAddrRanges() const;

    };

    /* Incoming port, for multi-ported controller add a crossbar
    * in front of it
    */
    MemoryPort port;

    /**
     * Remember if we have to retry a request when available.
     */
    bool retryRdReq;
    bool retryWrReq;


    bool hasPWQ;

    /* Peak bandwidth for reads and writes */
    const double readBandwidth;
    const double writeBandwidth;

    /**
     * Latency from that a request is accepted until the response is
     * ready to be sent.
     */
    const Tick readLatency;
    const Tick writeLatency;

    const Tick readProcessingLatency;
    const Tick writeProcessingLatency;

    const Tick frontendLatency;

    /**
     * Bandwidth and latency together allows us to size queues using
     * little's law
     */
    uint32_t readBufferSize;
    uint32_t writeBufferSize;

    const uint32_t burstLength;
    const uint32_t channels;

    // Thresholds for deciding when to switch from reads to writes
    uint32_t writeHighThreshold;
    uint32_t writeLowThreshold;

    uint16_t numCores;

    std::vector<bool> nackPkts;

    class BurstHelper {
        public:
            /** Number of PMEM bursts requred for a system packet **/
            const unsigned int burstCount;

            /** Number of PMEM bursts serviced so far for a system packet **/
            unsigned int burstsServiced;

        BurstHelper(unsigned int _burstCount)
            : burstCount(_burstCount), burstsServiced(0)
        { }
    };

    /**
     * PMEM packet stores packets along with timestamp of entry
     * in the queue, and time till exit. Based on PMEM packets
     */
    class PMEMPacket
    {
      public:

        /** When did request enter the controller */
        const Tick entryTime;

        /** When will request leave the controller */
        Tick readyTime;

        /** This comes from the outside world */
        PacketPtr pkt;

        const bool isRead;

        /**
         * The starting address of the PMEM packet.
         * This address could be unaligned to burst size boundaries. The
         * reason is to keep the address offset so we can accurately check
         * incoming read packets with packets in the write queue.
         */
        Addr addr;

        /**
         * The size of this pmem packet in bytes
         * It is always equal or smaller than PMEM burst size
         */
        unsigned int size;

        /**
         * A pointer to the BurstHelper if this PMEMPacket is a split packet
         * If not a split packet (common case), this is set to NULL
         */
        BurstHelper* burstHelper;

        PMEMPacket(PacketPtr _pkt, Addr _addr, unsigned int _size)
            : entryTime(curTick()), readyTime(curTick()), pkt(_pkt),
              isRead(_pkt->isRead()), addr(_addr), size(_size),
              burstHelper(NULL)
        {}
    };

    class URRecord
    {
        public:

        Addr addr;

        uint8_t *data;

        std::vector<bool> mask;

        unsigned size;

        PortID coreID;

        timestamp TS;

        URRecordType type;

        URRecord(PacketPtr pkt, URRecordType _type)
            : type(_type)
        {
            SenderInfo *s = dynamic_cast<SenderInfo *>(pkt->senderState);

            addr = pkt->getAddr();
            data = new uint8_t[BLK_SIZE];
            if (type == REDO)
                memcpy(data, pkt->getConstPtr<uint8_t>(), BLK_SIZE);
            mask = pkt->getMask();
            size = pkt->getSize();
            coreID = s->coreID;
            TS = s->TS;
        }

        ~URRecord()
        {
            if (data)
                delete[] data;
        }
    };

    std::vector<URRecord *> URTable;

    int URTCapacity;

    /**
     * Used for debugging to observe the contents of the queues.
     */
    void printQs() const;

    /**
     * Burst-align an address.
     *
     * @param addr The potentially unaligned address
     *
     * @return An address aligned to a PMEM burst
     */
    Addr burstAlign(Addr addr) const
    {
        return (addr & ~(Addr(burstLength - 1)));
    }


    /**
     * The controller's main read and write queues
     */
    std::deque<PMEMPacket*> readQueue;
    std::deque<PMEMPacket*> writeQueue;

    /**
     * To avoid iterating over the write queue to check for
     * overlapping transactions, maintain a set of burst addresses
     * that are currently queued. Since we merge writes to the same
     * location we never have more than one address to the same burst
     * address.
     */
    std::unordered_set<Addr> isInWriteQueue;

    /**
     * Response queues where packets wait after we're done working
     * with them, but it's not time to send the response yet. The
     * responses are stored seperately mostly to keep the code clean
     * and help with events scheduling. For all logical purposes such
     * as sizing the read queue, this and the requests queue need to
     * be added together.
     */
    std::deque<PMEMPacket*> readRespQueue;
    std::deque<PMEMPacket*> writeRespQueue;

    /* Handle incoming requests */
    BusState selectNextReq();
    void processNextReqEvent();
    EventFunctionWrapper nextReqEvent;

    /* Handle outgoing requests */
    /*
    void processRespondEvent();
    EventFunctionWrapper respondEvent;
    */

    /**
     * Check if the read queue has room for more entries
     *
     * @param pktCount The number of entries needed in the read queue
     * @return true if read queue is full, false otherwise
     */
    bool readQueueFull(unsigned int pktCount) const;

    /**
     * Check if the write queue has room for more entries
     *
     * @param pktCount The number of entries needed in the write queue
     * @return true if write queue is full, false otherwise
     */
    bool writeQueueFull(unsigned int pktCount) const;

    /* Schedules writes and takes care of eliding writes as configured */
    void scheduleWrite();

    /**
     * When a new read comes in, first check if the write q has a
     * pending request to the same address. If not, create one or multiple
     * PMEMPkts, and push them to the back of the read queue.\
     * If this is the only read request in the system, schedule an event to
     * start servicing it.
     *
     * @param pkt The request packet from the outside world
     * @param pktCount The number of PMEM bursts the pkt
     * translate to. If pkt size is larger then one full burst,
     * then pktCount is greater than one.
     */
    void addToReadQueue(PacketPtr pkt, unsigned int pktCount);

    /**
     * Decode the incoming pkt, create a pmem_pkt and push to the
     * back of the write queue. If the write q length is more than
     * the threshold specified by the user, ie the queue is beginning
     * to get full, stop reads, and start draining writes.
     *
     * @param pkt The request packet from the outside world
     * @param pktCount The number of PMEM bursts the pkt
     * translate to. If pkt size is larger then one full burst,
     * then pktCount is greater than one.
     */
    void addToWriteQueue(PacketPtr pkt, unsigned int pktCount);

    void addReadWriteRequest(PacketPtr pkt, unsigned int pktCount,
                            URRecord *rec);

    /**
     * Actually do the PMEM access - based on the latency it
     * will take to service the req, update the packet's "readyTime"
     * and move it to the response q from where it will eventually go back
     * to the outside world.
     *
     * @param pkt The PMEM packet created from the outside world pkt
     */
    void doPMEMAccess(PMEMPacket* pmem_pkt);

    /**
     * Uses the "access()" method in AbstractMemory to actually
     * create the response packet, and send it back to the outside
     * world requestor after adding any header or payload latency.
     *
     * @param pkt The packet from the outside world
     */
    void respond(PacketPtr pkt, Tick queue_delay);

    void handleEpochCompletion(PortID core, timestamp TS);

    BusState busState;

    /**
     * Track the state of the memory as either idle or busy, no need
     * for an enum with only two states.
     */
    bool isBusy;


    /**
     * Upstream caches need this packet until true is returned, so
     * hold it for deletion until a subsequent call
     */
    std::unique_ptr<Packet> pendingDelete;

    /**
     * Memory controller configuration initialized based on parameter
     * values.
     */
    Enums::PMemSched memSchedPolicy;
    Enums::AddrMap1 addrMapping;
    Enums::PageManage1 pageMgmt; //TODO

    // Latency of every nth write elided to figure out benefit of reducing
    // write traffic
    uint32_t elidedWrite;
    uint32_t elidedWriteCount;

    // All statistics that the model needs to capture
    Stats::Scalar readReqs;
    Stats::Scalar writeReqs;
    Stats::Scalar nonElidedWriteReqs;
    Stats::Scalar readBursts;
    Stats::Scalar writeBursts;
    Stats::Scalar bytesReadPMEM;
    Stats::Scalar bytesReadWrQ;
    Stats::Scalar bytesWritten;
    Stats::Scalar bytesReadSys;
    Stats::Scalar bytesWrittenSys;
    Stats::Scalar servicedByWrQ;
    Stats::Scalar mergedWrBursts;
    Stats::Scalar neitherReadNorWrite;
    Stats::Scalar numRdRetry;
    Stats::Scalar numWrRetry;
    Stats::Vector readPktSize;
    Stats::Vector writePktSize;
    //Stats::Vector rdQLenPdf;
    //Stats::Vector wrQLenPdf;
    Stats::Histogram rdPerTurnAround;
    Stats::Histogram wrPerTurnAround;

    // Latencies summed over all requests
    Stats::Scalar totRdQLat;
    Stats::Scalar totWrQLat;
    Stats::Scalar totMemAccLat;
    Stats::Scalar totRdLat;
    Stats::Scalar totWrLat;
    Stats::Scalar totBusLat;

    // Average latencies per request
    Stats::Formula avgRdQLat;
    Stats::Formula avgWrQLat;
    Stats::Formula avgBusLat;
    Stats::Formula avgRdLat;
    Stats::Formula avgWrLat;

    // Average bandwidth
    Stats::Formula avgRdBW;
    Stats::Formula avgWrBW;
    Stats::Formula avgRdBWSys;
    Stats::Formula avgWrBWSys;
    Stats::Formula peakWriteBW;
    Stats::Formula peakReadBW;
    Stats::Formula busUtil;
    Stats::Formula busUtilRead;
    Stats::Formula busUtilWrite;

    // Average queue lengths
    Stats::Average avgRdQLen;
    Stats::Average avgWrQLen;

    // Undo Redo Table stats
    Stats::Scalar totSpecWrites;
    Stats::Scalar totUndo;
    Stats::Scalar totRedo;
    Stats::Scalar totsafeWrAlias;
    Stats::Scalar totnumCoalesced;
    Stats::Scalar totRedoUndoAlias;
    Stats::Histogram urtOccupancy;

  public:

    void regStats() override;

    PMEMCtrl(const PMEMCtrlParams *p);

    DrainState drain() override;

    Port& getPort(const std::string& if_name,
                                PortID idx = InvalidPortID);

    void init() override;
/*    virtual void startup() override;
    virtual void drainResume() override;
*/
  protected:

    Tick recvAtomic(PacketPtr pkt);
    void recvFunctional(PacketPtr pkt);
    bool recvTimingReq(PacketPtr pkt);

};

#endif //__PMEM_CTRL_HH__
