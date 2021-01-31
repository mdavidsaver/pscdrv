
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <string.h>
#include <limits.h>

#include <osiSock.h>
#include <osiUnistd.h>
#include <errlog.h>
#include <epicsStdio.h>
#include <epicsAtomic.h>
#include <epicsTime.h>
#include <drvSup.h>
#include <iocsh.h>

#include <psc/device.h>

#include <epicsExport.h>

#if defined(_WIN32) || defined(__rtems__) || defined(vxWorks)
#  error Uses Linux / *BSD specific socket features
#endif

namespace {

// max size to allocate for a single buffer (Bytes)
int PSCUDPMaxPacketSize = 1024;
// max RX packet rate (pkt/sec)
double PSCUDPMaxPacketRate = 280000.0;
// max time between disk flush (sec)
double PSCUDPMaxFlushPeriod = 1.0;
// max data file size before rotation (MB)
double PSCUDPMaxLenMB = 2000;

// OS limit on maximum number of iovec passed to writev
#ifndef IOV_MAX
// conservative default for antique Linux (see NOTES for man writev)
#  define IOV_MAX (16)
#endif
size_t iovLimit = IOV_MAX;

struct DataFD {
    int fd;

    DataFD() :fd(-1) {}
    ~DataFD() { close(); }
    void close() {
        if(fd>=0)
            ::close(fd);
        fd = -1;
    }
    bool isOpen() const { return fd>=0; }

private:
    DataFD(const DataFD&);
    DataFD& operator=(const DataFD&);
};

struct UDPFast : public PSCBase
{
    SOCKET sock;
    osiSockAddr self, peer;

    int running;
    size_t batchSize;

    typedef std::vector<std::vector<char> > vecs_t;
    // vector data free-list
    // entries originating with this free-list may appear in:
    //   vpool
    //   pending
    //   inprog - local to rxfn()
    // guarded by rxLock
    vecs_t vpool;

    struct pkt {
        std::vector<char> body;
        size_t bodylen;
        epicsTimeStamp rxtime;
        epicsUInt16 msgid;
    };

    // guarded by rxLock
    typedef std::vector<pkt> pkts_t;
    pkts_t pending;

    epicsEvent vpoolStall;
    epicsEvent pendingReady; // set from rxWorker to wake cacheWorker

    std::string filebase;
    bool reopen;

    // rx worker pulls from socket buffer and pushes to 'pending'
    struct RXWorker : public epicsThreadRunable
    {
        UDPFast * const self;
        explicit RXWorker(UDPFast* self) : self(self) {}
        virtual ~RXWorker() {}
        virtual void run() override final { self->rxfn(); }
    } rxjob;
    epicsThread rxworker;
    mutable epicsMutex rxLock;

    // cache worker pulls from 'pending' and pushes to Block cache
    struct CacheWorker : public epicsThreadRunable
    {
        UDPFast *self;
        explicit CacheWorker(UDPFast* self) : self(self) {}
        virtual ~CacheWorker() {}
        virtual void run() override final { self->cachefn(); }
    } cachejob;
    epicsThread cacheworker;

    UDPFast(const std::string& name,
            const std::string& host,
            unsigned short port,
            unsigned short bindport)
        :PSCBase (name, host, port)
        ,sock(epicsSocketCreate(AF_INET, SOCK_DGRAM, 0))
        ,running(1)
        ,filebase("./testdata-")
        ,reopen(true)
        ,rxjob(this)
        ,rxworker(rxjob, "udpfrx", epicsThreadGetStackSize(epicsThreadStackBig), epicsThreadPriorityHigh+1)
        ,cachejob(this)
        ,cacheworker(cachejob, "udpfc", epicsThreadGetStackSize(epicsThreadStackBig), epicsThreadPriorityHigh-1)
    {
        if(sock==INVALID_SOCKET)
            throw std::bad_alloc();

        {
            timeval timeout = {1, 0};
            if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)))
                throw std::runtime_error("Unable to set SO_RCVTIMEO");
        }
        {
            int flag = 6; // highest non-privileged
            if(setsockopt(sock, SOL_SOCKET, SO_PRIORITY, &flag, sizeof(flag)))
                fprintf(stderr, "Unable to set SO_PRIORITY");
        }
        {
            int flag = 1;
            if(setsockopt(sock, SOL_SOCKET, SO_RXQ_OVFL, &flag, sizeof(flag)))
                fprintf(stderr, "Unable to set SO_RXQ_OVFL");
        }
        // TODO: set SO_RCVBUF, SO_INCOMING_CPU, SO_BUSY_POLL ?

        unsigned rxbuflen = 0; // in bytes
        {
            osiSocklen_t len = sizeof(rxbuflen);
            if(getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rxbuflen, &len)) {
                fprintf(stderr, "Unable to get SO_RCVBUF");
            } else {
                printf("  SO_RCVBUF = %u\n", rxbuflen);
            }
        }

        const unsigned maxpktlen = std::max(8, PSCUDPMaxPacketSize);

        // recvmmsg() can only deque as many as can fit it the socket buffer
        // Not considering that Linux _may_ apply a 2x multiplier
        batchSize = std::min(std::max<size_t>(1u, rxbuflen/maxpktlen), iovLimit);
        printf("  batch size %zu\n", batchSize);

        // pre-allocate buffers to handle 2 periods of data.
        // one accumulating, and another flushing
        vpool.resize(size_t(std::max(1.0, 2*PSCUDPMaxPacketRate*PSCUDPMaxFlushPeriod)));
        for(size_t i=0; i<vpool.size(); i++)
            vpool[i].resize(maxpktlen);
        printf("  vpool cnt=%zu size=%u b\n", vpool.size(), maxpktlen);

        pending.reserve(vpool.size());

        if(aToIPAddr(host.c_str(), port, &peer.ia))
            throw std::runtime_error("Bad host/IP");

        memset(&self, 0, sizeof(self));
        self.ia.sin_family = AF_INET;
        self.ia.sin_addr.s_addr = htonl(INADDR_ANY);
        self.ia.sin_port = htons(bindport);

        if(bind(sock, &self.sa, sizeof(self.ia)))
            throw std::runtime_error("Unable to bind()");

        {
            osiSocklen_t len = sizeof(self);
            if(getsockname(sock, &self.sa, &len))
                throw std::runtime_error("Unable to getsockname()");
        }
    }

    virtual ~UDPFast()
    {
        epicsSocketDestroy(sock);
    }

    void rxfn() {
        if(PSCDebug>=2)
            errlogPrintf("%s : rx worker starts\n", name.c_str());

        epicsUInt32 prevndrops = 0u;

        struct message {
            std::vector<char> buf; // body buffer (swapped out frequently)
            osiSockAddr src;
            iovec io[2]; // receive header and body into separate buffers
            union {
                struct {
                    char P, S;
                    epicsUInt16 msgid;
                    epicsUInt32 blen;
                };
                char hbuf[8]; // header buffer
            };
            union {
                cmsghdr _calign; // CMSG_* access macros assume alignment
                char cbuf[CMSG_SPACE(4u)]; // space for SO_RXQ_OVFL
            };
        };

        std::vector<mmsghdr> headers(batchSize);
        std::vector<message> msgs(headers.size());
        bool notifycache = false;

        Guard G(rxLock);

        // loop to receive batches of packets
        while(epics::atomic::get(running)) { // main rx loop

            if(vpool.empty()) {
                if(PSCDebug>=1)
                    errlogPrintf("%s : vpool stall\n", name.c_str());

                UnGuard U(G);
                vpoolStall.wait();
                continue;
            }

            // assign buffers
            size_t nassign = msgs.size();
            for(size_t i=0; i<nassign; i++) {
                msghdr& hdr = headers[i].msg_hdr;
                message& msg = msgs[i];

                if(!msg.buf.empty()) {
                    // re-use leftovers

                } else if(vpool.empty()) {
                    nassign = i;
                    break;

                } else {
                    msg.buf.swap(vpool.back());
                    vpool.pop_back();
                    assert(msg.buf.size()>=8);

                    msg.io[1].iov_base = &msg.buf[0];
                    msg.io[1].iov_len = msg.buf.size();
                }

                headers[i].msg_len = 0u;

                hdr.msg_name = &msg.src;
                hdr.msg_namelen = sizeof(msg.src);
                hdr.msg_flags = 0u;
                hdr.msg_control = &msg.cbuf;
                hdr.msg_controllen = sizeof(msg.cbuf);
                hdr.msg_iov = msg.io;
                hdr.msg_iovlen = 2u;

                msg.io[0].iov_base = msg.hbuf;
                msg.io[0].iov_len = sizeof(msg.hbuf);
            }

            if(nassign < msgs.size()) {
                if(PSCDebug>=2)
                    errlogPrintf("%s : insufficient buffers for for recvmmsg %zu < %zu\n",
                                 name.c_str(), nassign, msgs.size());

            } else {
                if(PSCDebug>=5)
                    errlogPrintf("%s nassign=%zu vpool=%zu\n", name.c_str(), nassign, vpool.size());
            }

            size_t nrx = 0u;
            {
                UnGuard U(G);

                if(notifycache) {
                    if(PSCDebug>=4)
                        errlogPrintf("%s notify\n", name.c_str());
                    pendingReady.signal();
                    notifycache = false;
                }

                if(nassign==0) {
                    // ouch.  out of buffers.
                    epicsThreadSleep(1.0);
                    // fall through for retry

                } else {
                    int ret = recvmmsg(sock, &headers[0], nassign, MSG_WAITFORONE, 0);

                    int lvl = 5;
                    if(ret<0)
                        lvl = 1;
                    else if(size_t(ret)==nassign)
                        lvl = 2; // could have used larget PSCUDPBatchSize
                    if(PSCDebug >= lvl)
                        errlogPrintf("%s : recvmmsg() -> %d (%d)\n", name.c_str(), ret, int(SOCKERRNO));

                    if(ret < 0) {
                        if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINPROGRESS) {
                            if(PSCDebug>=2)
                                errlogPrintf("%s : recvmmsg() timeout\n", name.c_str());

                        } else {
                            if(PSCDebug>=0)
                                errlogPrintf("%s : recvmmsg() error (%d) %s\n", name.c_str(), errno, strerror(errno));
                        }

                    } else {
                        nrx = size_t(ret);
                    }
                }
                // re-lock
            }
            epicsTimeStamp rxtime;
            // all messages in a batch will have the same RX time
            epicsTimeGetCurrent(&rxtime);

            for(size_t i=0; i<nrx; i++) { // for each received packet
                msghdr& hdr = headers[i].msg_hdr;
                size_t len = headers[i].msg_len;
                message& msg = msgs[i];
                epicsUInt32 ndrops = 0;

                if(hdr.msg_flags & MSG_CTRUNC) {
                    // this will absolutely spam the console, but represents a logic error in sizing msg.cbuf
                    if(PSCDebug>0)
                        errlogPrintf("%s : MSG_CTRUNC\n", name.c_str());
                }

                // process drop count even if this isn't a valid peer message
                for(cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr); cmsg; cmsg = CMSG_NXTHDR(&hdr, cmsg)) {
                    // Linux omits message when count is zero.
                    if(cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type == SO_RXQ_OVFL && cmsg->cmsg_len>=CMSG_LEN(4u)) {
                        memcpy(&ndrops, CMSG_DATA(cmsg), sizeof(ndrops));
                        if(ndrops!=prevndrops) {
                            // assuming SO_RXQ_OVFL messages will be in order since they originate within the OS
                            if(PSCDebug>=1)
                                errlogPrintf("%s : socket buffer overflow.  lost %u\n", name.c_str(), ndrops-prevndrops);
                            prevndrops = ndrops;
                        }
                    }
                }

                if(evutil_sockaddr_cmp(&peer.sa, &msg.src.sa, 1)!=0) {
                    if(PSCDebug>0)
                        errlogPrintf("%s : ignore packet not from peer\n", name.c_str());
                    continue;

                } else if(len<8u) {
                    if(PSCDebug>=0)
                        errlogPrintf("%s : truncated packet\n", name.c_str());
                    continue;

                } else if(msg.P!='P' || msg.S!='S') {
                    if(PSCDebug>=0)
                        errlogPrintf("%s : invalid header packet\n", name.c_str());
                    continue;
                }

                epicsUInt16 msgid = ntohs(msg.msgid);
                epicsUInt32 blen = ntohl(msg.blen);

                if(blen < len-8u) {
                    if(PSCDebug>=0)
                        errlogPrintf("%s : truncated packet body %u > %u\n", name.c_str(),
                                     unsigned(blen), unsigned(len-8u));
                    continue;
                }

                notifycache |= pending.empty();
                // will signal after unlock to avoid bouncing

                pending.push_back(pkt());
                pending.back().msgid = msgid;
                pending.back().rxtime = rxtime;
                pending.back().body.swap(msg.buf);
                pending.back().bodylen = blen;
            }
        } // main rx

        if(PSCDebug>=2)
            errlogPrintf("%s : rx worker ends\n", name.c_str());
    } // rxfn()

    void cachefn()
    {
        if(PSCDebug>=2)
            errlogPrintf("%s : cache worker starts\n", name.c_str());

        DataFD datafile;

        struct header_t {
            const char P, S;
            epicsUInt16 msgid;
            epicsUInt32 bodylen;
            epicsUInt32 sec;
            epicsUInt32 nsec;
            header_t() :P('P'), S('S') {}
        };
        std::vector<iovec> ios(iovLimit); // round down to multiple of 2
        std::vector<header_t> headers(ios.size()/2u);

        pkts_t inprog;
        {
            Guard R(rxLock);
            inprog.reserve(pending.capacity());
            // our 'inprog' and 'pending' are swap()'d as two parts of a double buffering scheme
        }

        Guard G(lock);

        while(true) {
            {
                UnGuard U(G);

                // de-assign
                bool unstall = false;
                if(!inprog.empty()) {
                    Guard R(rxLock);

                    unstall = vpool.empty();

                    for(size_t i=0, N=inprog.size(); i<N; i++) {
                        pkt& pkt = inprog[i];

                        if(!pkt.body.empty()) {
                            vpool.push_back(vecs_t::value_type()); // shouldn't need to (re)allocate
                            vpool.back().swap(pkt.body);
                            if(PSCDebug>=5)
                                errlogPrintf("%s : return consumed %zu\n", name.c_str(), i);
                        }
                    }
                    inprog.clear();

                    unstall &= !vpool.empty();
                }
                if(unstall) {
                    if(PSCDebug>=1)
                        errlogPrintf("%s : vpool stall resume\n", name.c_str());
                    vpoolStall.signal();
                }

                if(!epics::atomic::get(running))
                    break;

                pendingReady.wait();

                {
                    // grab all pending
                    Guard R(rxLock);
                    inprog.swap(pending);
                }
            }

            if(PSCDebug>=5)
                errlogPrintf("%s : consuming %zu\n", name.c_str(), inprog.size());

            for(size_t i=0, N=inprog.size(); i<N; i++) {
                pkt& pkt = inprog[i];

                block_map::const_iterator it=recv_blocks.find(pkt.msgid);
                if(it==recv_blocks.end()) {
                    ukncount++;

                } else {
                    Block* blk = it->second;
                    blk->count++;
                    blk->rxtime = pkt.rxtime;

                    blk->data.assign(&pkt.body[0], pkt.body.size());

                    blk->requestScan();
                    blk->listeners(blk);
                }
            }

            if(datafile.isOpen()) {
                off_t pos = lseek(datafile.fd, 0, SEEK_CUR);
                if(pos!=(off_t)-1 && pos>=off_t(PSCUDPMaxLenMB*(1u<<20u))) {
                    reopen = true;
                    if(PSCDebug>=2)
                        errlogPrintf("%s : rotate data file for size=%zu\n", name.c_str(), size_t(pos));
                }
            }

            if(reopen && !filebase.empty()) {
                reopen = false;

                epicsTimeStamp now;
                std::string fname;

                fname = filebase;

                UnGuard U(G);

                // hack: if this is a re-open after an error, ensure that the file name will be different
                epicsThreadSleep(1.0);

                epicsTimeGetCurrent(&now);
                char tsbuf[25];
                epicsTimeToStrftime(tsbuf, sizeof(tsbuf), "%Y%m%d-%H%M%S", &now);

                fname += tsbuf;
                fname += ".dat";

                datafile.close();

                datafile.fd = ::open(fname.c_str(), O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0644);
                if(datafile.fd==-1) {
                    errlogPrintf("%s : Error opening \"%s\" : (%d) %s\n",
                                 name.c_str(), fname.c_str(), errno, strerror(errno));
                } else {
                    if(PSCDebug>=1)
                        errlogPrintf("%s : opened \"%s\"\n", name.c_str(), fname.c_str());
                }
            }

            if(!datafile.isOpen())
                continue;

            UnGuard U(G);

            epicsUInt64 tstart = epicsMonotonicGet();
            size_t datatotal = 0u;

            // iterate inprog and write in batches
            for(size_t i=0, N=inprog.size(); i<N && datafile.isOpen();) {
                size_t batchtotal = 0u;
                size_t b, B;

                for(b=0, B=headers.size(); i<N && b<B; i++, b++) {
                    auto& pkt = inprog[i];
                    auto& H = headers[b];
                    auto& IOhead = ios[2*b+0];
                    auto& IObody = ios[2*b+1];

                    H.msgid = htons(pkt.msgid);
                    H.bodylen = htonl(pkt.bodylen);
                    H.sec = htonl(pkt.rxtime.secPastEpoch + POSIX_TIME_AT_EPICS_EPOCH);
                    H.nsec = htonl(pkt.rxtime.nsec);

                    IOhead.iov_base = &H;
                    IOhead.iov_len = sizeof(header_t);
                    IObody.iov_base = &pkt.body[0];
                    IObody.iov_len = pkt.bodylen;
                    batchtotal += sizeof(header_t) + pkt.bodylen;
                }

                ssize_t ret = writev(datafile.fd, &ios[0], 2*b);
                if(ret<0) {
                    if(PSCDebug>=0)
                        errlogPrintf("%s : data file write error: (%d) %s\n", name.c_str(), errno, strerror(errno));
                    datafile.close();
                    reopen = true;

                } else if(size_t(ret)!=batchtotal) {
                    if(PSCDebug>=0)
                        errlogPrintf("%s : data file write incomplete %zd of %zu\n", name.c_str(), ret, batchtotal);
                    datafile.close();
                    reopen = true;
                }

                datatotal += batchtotal;
            }

            if(datafile.isOpen() && fdatasync(datafile.fd)) {
                if(PSCDebug>=0)
                    errlogPrintf("%s : fdatasync() error %d\n", name.c_str(), errno);
            }

            epicsUInt64 tend = epicsMonotonicGet();
            if(PSCDebug>=3) {
                double ellapsed = (tend-tstart)/1e9; // sec
                double rate = datatotal/ellapsed; // B/s

                errlogPrintf("%s : data file wrote %zu B in %g ms for %.3g GB/s\n",
                             name.c_str(), datatotal, ellapsed*1e3, rate/double(1u<<30u));
            }

        }

        if(PSCDebug>=2)
            errlogPrintf("%s : cache worker ends\n", name.c_str());
    }

    virtual void connect() override final
    {
        connected = true;
        rxworker.start();
        cacheworker.start();
    }
    virtual void stop() override final
    {
        connected = false;
        epics::atomic::set(running, 0);
        {
            // send a zero length packet to myself to wake rxworker
            char junk = 0;
            if(sendto(sock, &junk, 0, 0, &self.sa, sizeof(self))<0)
                errlogPrintf("%s : error waking rxworker\n", name.c_str());
        }
        vpoolStall.signal();
        pendingReady.signal(); // wake cacheworker
        rxworker.exitWait();
        cacheworker.exitWait();
    }

    virtual void queueSend(epicsUInt16, const void *, epicsUInt32) override final {}
    virtual void queueSend(Block *, const dbuffer &) override final {}
    virtual void queueSend(Block *, const void *, epicsUInt32) override final {}
    virtual void flushSend() override final {}
    virtual void forceReConnect() override final {}

    virtual void report(int lvl) override final {}
};


void createPSCUDPFast(const char* name, const char* host, int hostport, int ifaceport)
{
    try {
        (void)new UDPFast(name, host, hostport, ifaceport);
    }catch(std::exception& e){
        fprintf(stderr, "Error: %s\n", e.what());
    }
}

const iocshArg createPSCUDPFastArg0 = {"name", iocshArgString};
const iocshArg createPSCUDPFastArg1 = {"hostname", iocshArgString};
const iocshArg createPSCUDPFastArg2 = {"hostport#", iocshArgInt};
const iocshArg createPSCUDPFastArg3 = {"ifaceport#", iocshArgInt};
const iocshArg * const createPSCUDPFastArgs[] =
{&createPSCUDPFastArg0,&createPSCUDPFastArg1,&createPSCUDPFastArg2,&createPSCUDPFastArg3};
const iocshFuncDef createPSCUDPFastDef = {"createPSCUDPFast", 4, createPSCUDPFastArgs};
void createPSCUDPFastArgsCallFunc(const iocshArgBuf *args)
{
    createPSCUDPFast(args[0].sval, args[1].sval, args[2].ival, args[3].ival);
}

void pscudp()
{
    iocshRegister(&createPSCUDPFastDef, &createPSCUDPFastArgsCallFunc);

    auto lim = sysconf(_SC_IOV_MAX);
    if(lim>0)
        iovLimit = lim;
}

bool report1(int lvl, const PSCBase* base)
{
    auto drv = dynamic_cast<const UDPFast*>(base);
    if(!drv)
        return true;

    printf("PSCUDP: %s\n", drv->name.c_str());

    if(lvl<=0)
        return true;

    size_t vpoolCnt, pendingCnt;
    if(lvl>0) {
        Guard G(drv->rxLock);
        vpoolCnt = drv->vpool.size();
        pendingCnt = drv->pending.size();
    }
    printf("  vpool#=%zu pending#=%zu\n", vpoolCnt, pendingCnt);

    return true;
}

long report(int lvl)
{
    PSCBase::visit(report1, lvl);
    return 0;
}

drvet drvUDPFast = {
    2,
    (DRVSUPFUN)report,
    0,
};

} // namespace

extern "C" {
epicsExportRegistrar(pscudp);
epicsExportAddress(drvet, drvUDPFast);
epicsExportAddress(int, PSCUDPMaxPacketSize);
epicsExportAddress(double, PSCUDPMaxPacketRate);
epicsExportAddress(double, PSCUDPMaxFlushPeriod);
epicsExportAddress(double, PSCUDPMaxLenMB);
}
