from m5.params import *
from m5.objects.AbstractMemory import *

# Enum for memory scheduling algorithms, currently First-Come
# First-Served and a First-Row Hit then First-Come First-Served
class PMemSched(Enum): vals = ['writeHigh', 'writeHighLow']

# Enum for the address mapping. With Ch, Ra, Ba, Ro and Co denoting
# channel, rank, bank, row and column, respectively, and going from
# MSB to LSB.  Available are RoRaBaChCo and RoRaBaCoCh, that are
# suitable for an open-page policy, optimising for sequential accesses
# hitting in the open row. For a closed-page policy, RoCoRaBaCh
# maximises parallelism.
class AddrMap1(Enum): vals = ['RoRaBaChCo', 'RoRaBaCoCh', 'RoCoRaBaCh']

# Enum for the page policy, either open, open_adaptive, close, or
# close_adaptive.
class PageManage1(Enum): vals = ['open', 'open_adaptive', 'close',
                                'close_adaptive']

class PMEMCtrl(AbstractMemory):
    type = 'PMEMCtrl'
    cxx_header = "mem/pmem_ctrl.hh"

    # single-ported on the system interface side, instantiate with a
    # bus in front of the controller for multiple ports
    port = SlavePort("Slave port")

    # threshold in percent for when to forcefully trigger writes and
    # start emptying the write buffer
    write_high_thresh_perc = Param.Percent(60, "Threshold to force writes")

    # threshold in percentage for when to start writes if the read
    # queue is empty
    write_low_thresh_perc = Param.Percent(0, "Threshold to start writes")

    # minimum write bursts to schedule before switching back to reads
    min_writes_per_switch = Param.Unsigned(16, "Minimum write bursts before "
                                           "switching to reads")

    burst_length = Param.Unsigned(64, "Burst lenght (BL) in beats")

    channels = Param.Unsigned(1, "Number of channels")

    num_cores = Param.Unsigned(1, "Number of cores connected to the ctrl")

    urt_capacity = Param.Unsigned(64, "Maximum size of Undo Redo Table")

    # Is the write request queue persistent?
    pwq = Param.Bool(True, "Is write request queue persistent?")

    # pipeline latency of the controller and PHY, split into a
    # frontend part and a backend part, with reads and writes serviced
    # by the queues only seeing the frontend contribution, and reads
    # serviced by the memory seeing the sum of the two
    frontend_latency = Param.Latency("10ns", "Time spent in frontend of MC")
    read_latency = Param.Latency("175ns", "Latency of reads accessing memory")
    write_latency = Param.Latency("60ns", "Latency of writes accessing memory")

    read_bandwidth = Param.MemoryBandwidth('6.6GB/s',
                                      "Read bandwidth")

    write_bandwidth = Param.MemoryBandwidth('2.3GB/s',
                                      "Write bandwidth")

    # only used for the address mapping as the controller by
    # construction is a single channel and multiple controllers have
    # to be instantiated for a multi-channel configuration
    channels = Param.Unsigned(1, "Number of channels")

    # scheduler, address map and page policy
    mem_sched_policy = Param.PMemSched('writeHigh', "Memory scheduling policy")
    addr_mapping = Param.AddrMap1('RoRaBaCoCh', "Address mapping policy")
    page_policy = Param.PageManage1('open_adaptive', "Page management policy")

    elided_write = Param.Unsigned(0, "Latency of every Nth write elided")

    # the base clock period of the PMEM
    # TODO: 2666 MHz, for now
    tCK = Param.Latency('0.375ns', "Clock period")
