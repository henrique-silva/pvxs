/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#if defined(_WIN32)
#  include <windows.h>
#elif !defined(__rtems__) && !defined(vxWorks)
#  include <sys/types.h>
#  include <unistd.h>
#endif

#include <list>
#include <map>
#include <regex>
#include <system_error>
#include <functional>
#include <atomic>
#include <cstdlib>

#include <signal.h>

#include <dbDefs.h>
#include <envDefs.h>
#include <epicsThread.h>
#include <epicsTime.h>
#include <epicsGuard.h>
#include <epicsString.h>

#include <pvxs/server.h>
#include <pvxs/log.h>
#include "evhelper.h"
#include "serverconn.h"
#include "utilpvt.h"
#include "udp_collector.h"

namespace pvxs {
namespace server {
using namespace impl;

DEFINE_LOGGER(serversetup, "pvxs.server.setup");
DEFINE_LOGGER(serverio, "pvxs.server.io");

namespace {
void split_addr_into(const char* name, std::vector<std::string>& out, const char *inp)
{
    std::regex word("\\s*(\\S+)(.*)");
    std::cmatch M;

    while(*inp && std::regex_match(inp, M, word)) {
        sockaddr_in addr = {};
        if(aToIPAddr(M[1].str().c_str(), 0, &addr)) {
            log_err_printf(serversetup, "%s ignoring invalid '%s'\n", name, M[1].str().c_str());
            continue;
        }
        char buf[24];
        ipAddrToDottedIP(&addr, buf, sizeof(buf));
        out.emplace_back(buf);
        inp = M[2].first;
    }
}

const char* pickenv(const char** picked, std::initializer_list<const char*> names)
{
    for(auto name : names) {
        if(auto val = getenv(name)) {
            if(picked)
                *picked = name;
            return val;
        }
    }
    return nullptr;
}

} // namespace

Config Config::from_env()
{
    Config ret;
    ret.udp_port = 5076;

    const char* name;

    if(const char *env = pickenv(&name, {"EPICS_PVAS_INTF_ADDR_LIST"})) {
        split_addr_into(name, ret.interfaces, env);
    }

    if(auto env = pickenv(&name, {"EPICS_PVAS_BEACON_ADDR_LIST", "EPICS_PVA_ADDR_LIST"})) {
        split_addr_into(name, ret.beaconDestinations, env);
    }

    if(const char *env = pickenv(&name, {"EPICS_PVAS_AUTO_BEACON_ADDR_LIST", "EPICS_PVA_AUTO_ADDR_LIST"})) {
        if(epicsStrCaseCmp(env, "YES")==0) {
            ret.auto_beacon = true;
        } else if(epicsStrCaseCmp(env, "NO")==0) {
            ret.auto_beacon = false;
        } else {
            log_err_printf(serversetup, "%s invalid bool value (YES/NO)", name);
        }
    }

    if(const char *env = pickenv(&name, {"EPICS_PVAS_SERVER_PORT", "EPICS_PVA_SERVER_PORT"})) {
        try {
            ret.tcp_port = lexical_cast<unsigned short>(env);
        }catch(std::exception& e) {
            log_err_printf(serversetup, "%s invalid integer : %s", name, e.what());
        }
    }

    if(const char *env = pickenv(&name, {"EPICS_PVAS_BROADCAST_PORT", "EPICS_PVA_BROADCAST_PORT"})) {
        try {
            ret.udp_port = lexical_cast<unsigned short>(env);
        }catch(std::exception& e) {
            log_err_printf(serversetup, "%s invalid integer : %s", name, e.what());
        }
    }

    return ret;
}


Server Config::build()
{
    Server ret(std::move(*this));
    return ret;
}

Server::Server() {}

Server::Server(Config&& conf)
{
    /* Here be dragons.
     *
     * We keep two different ref. counters.
     * - "externel" counter which keeps a server running.
     * - "internal" which only keeps server storage from being destroyed.
     *
     * External refs are held as Server::pvt.  Internal refs are
     * held by various in-progress operations (OpBase sub-classes)
     * Which need to safely access server storage, but should not
     * prevent a server from stopping.
     */
    auto internal(std::make_shared<Pvt>(std::move(conf)));
    internal->internal_self = internal;

    // external
    pvt.reset(internal.get(), [internal](Pvt*) mutable {
        internal->stop();
        internal.reset();
    });
    // we don't keep a weak_ptr to the external reference.
    // Caller is entirely responsible for keeping this server running
}

Server::~Server() {}

Server& Server::addSource(const std::string& name,
                  const std::shared_ptr<Source>& src,
                  int order)
{
    if(!pvt)
        throw std::logic_error("NULL Server");
    if(!src)
        throw std::logic_error(SB()<<"Attempt to add NULL Source "<<name<<" at "<<order);
    {
        auto G(pvt->sourcesLock.lockWriter());

        auto& ent = pvt->sources[std::make_pair(order, name)];
        if(ent)
            throw std::runtime_error(SB()<<"Source already registered : ("<<name<<", "<<order<<")");
        ent = src;
    }
    return *this;
}

std::shared_ptr<Source> Server::removeSource(const std::string& name,  int order)
{
    if(!pvt)
        throw std::logic_error("NULL Server");

    auto G(pvt->sourcesLock.lockWriter());

    std::shared_ptr<Source> ret;
    auto it = pvt->sources.find(std::make_pair(order, name));
    if(it!=pvt->sources.end()) {
        ret = it->second;
        pvt->sources.erase(it);
    }

    return ret;
}

std::shared_ptr<Source> Server::getSource(const std::string& name, int order)
{
    if(!pvt)
        throw std::logic_error("NULL Server");

    auto G(pvt->sourcesLock.lockReader());

    std::shared_ptr<Source> ret;
    auto it = pvt->sources.find(std::make_pair(order, name));
    if(it!=pvt->sources.end()) {
        ret = it->second;
    }

    return ret;
}

void Server::listSource(std::vector<std::pair<std::string, int> > &names)
{
    if(!pvt)
        throw std::logic_error("NULL Server");

    names.clear();

    auto G(pvt->sourcesLock.lockReader());

    names.reserve(pvt->sources.size());

    for(auto& pair : pvt->sources) {
        names.emplace_back(pair.first.second, pair.first.first);
    }
}

const Config& Server::config() const
{
    if(!pvt)
        throw std::logic_error("NULL Server");

    return pvt->effective;
}

Server& Server::start()
{
    if(!pvt)
        throw std::logic_error("NULL Server");
    pvt->start();
    return *this;
}

Server& Server::stop()
{
    if(!pvt)
        throw std::logic_error("NULL Server");
    pvt->stop();
    return *this;
}

static std::atomic<Server::Pvt*> sig_target{nullptr};

static void sig_handle(int sig)
{
    auto serv = sig_target.load();

    if(serv)
        serv->done.signal();
}

Server& Server::run()
{
    if(!pvt)
        throw std::logic_error("NULL Server");

    Server::Pvt* expect = nullptr;

    std::function<void()> cleanup;
    if(sig_target.compare_exchange_weak(expect, pvt.get())) {
        // we claimed the signal handler slot.
        // save previous handlers
        auto prevINT  = signal(SIGINT , &sig_handle);
        auto prevTERM = signal(SIGTERM, &sig_handle);

        cleanup = [this, prevINT, prevTERM]() {
            Server::Pvt* expect = pvt.get();
            if(sig_target.compare_exchange_weak(expect, nullptr)) {
                signal(SIGINT , prevINT);
                signal(SIGTERM, prevTERM);
            }
        };
    }

    try {
        pvt->start();

        pvt->done.wait();

        pvt->stop();
    } catch(...) {
        if(cleanup)
            cleanup();
        throw;
    }
    if(cleanup)
        cleanup();

    return *this;
}

Server& Server::interrupt()
{
    if(!pvt)
        throw std::logic_error("NULL Server");
    pvt->done.signal();
    return *this;
}

Server::Pvt::Pvt(Config&& conf)
    :effective(std::move(conf))
    ,beaconMsg(128)
    ,acceptor_loop("PVXTCP", epicsThreadPriorityCAServerLow-2)
    ,beaconSender(AF_INET, SOCK_DGRAM, 0)
    ,beaconTimer(event_new(acceptor_loop.base, -1, EV_TIMEOUT, doBeaconsS, this))
    ,searchReply(0x10000)
    ,state(Stopped)
{
    // empty interface address list implies the wildcard
    // (because no addresses isn't interesting...)
    if(effective.interfaces.empty()) {
        effective.interfaces.emplace_back("0.0.0.0");
    }

    auto manager = UDPManager::instance();

    for(const auto& iface : effective.interfaces) {
        SockAddr addr(AF_INET, iface.c_str());
        addr.setPort(effective.udp_port);
        listeners.push_back(manager.onSearch(addr,
                                             std::bind(&Pvt::onSearch, this, std::placeholders::_1) ));
        // update to allow udp_port==0
        effective.udp_port = addr.port();
    }

    evsocket dummy(AF_INET, SOCK_DGRAM, 0);

    acceptor_loop.call([this, &dummy](){
        // from acceptor worker

        bool firstiface = true;
        for(const auto& addr : effective.interfaces) {
            interfaces.emplace_back(addr, effective.tcp_port, this, firstiface);
            if(firstiface || effective.tcp_port==0)
                effective.tcp_port = interfaces.back().bind_addr.port();
            firstiface = false;
        }

        for(const auto& addr : effective.beaconDestinations) {
            beaconDest.emplace_back(AF_INET, addr.c_str(), effective.udp_port);
        }

        if(effective.auto_beacon) {
            // append broadcast addresses associated with our bound interface(s)

            ELLLIST bcasts = ELLLIST_INIT;

            try {
                for(const auto& iface : interfaces) {
                    if(iface.bind_addr.family()!=AF_INET)
                        continue;
                    osiSockAddr match;
                    match.ia = iface.bind_addr->in;
                    osiSockDiscoverBroadcastAddresses(&bcasts, dummy.sock, &match);
                }

                // do our best to avoid a bad_alloc during iteration
                beaconDest.reserve(beaconDest.size()+(size_t)ellCount(&bcasts));

                while(ELLNODE *cur = ellGet(&bcasts)) {
                    osiSockAddrNode *node = CONTAINER(cur, osiSockAddrNode, node);
                    beaconDest.emplace_back(AF_INET);
                    beaconDest.back()->in = node->addr.ia;
                    free(cur);
                }

            }catch(...){
                ellFree(&bcasts);
                throw;
            }
        }

        effective.interfaces.clear();
        for(const auto& iface : interfaces) {
            effective.interfaces.emplace_back(iface.bind_addr.tostring());
        }

        effective.beaconDestinations.clear();
        for(const auto& addr : beaconDest) {
            effective.beaconDestinations.emplace_back(addr.tostring());
        }

        effective.auto_beacon = false;
    });

    {
        // choose new GUID.
        // treat as 3x 32-bit unsigned.
        union {
            std::array<uint32_t, 3> i;
            std::array<uint8_t, 3*4> b;
        } pun;
        static_assert (sizeof(pun)==12, "");

        // i[0] (start) time
        epicsTimeStamp now;
        epicsTimeGetCurrent(&now);
        pun.i[0] = now.secPastEpoch ^ now.nsec;

        // i[1] host
        // mix together first interface and all local bcast addresses
        pun.i[1] = osiLocalAddr(dummy.sock).ia.sin_addr.s_addr;
        {
            ELLLIST bcasts = ELLLIST_INIT;
            osiSockAddr match;
            match.ia.sin_family = AF_INET;
            match.ia.sin_addr.s_addr = htonl(INADDR_ANY);
            match.ia.sin_port = 0;
            osiSockDiscoverBroadcastAddresses(&bcasts, dummy.sock, &match);

            while(ELLNODE *cur = ellGet(&bcasts)) {
                osiSockAddrNode *node = CONTAINER(cur, osiSockAddrNode, node);
                if(node->addr.sa.sa_family==AF_INET)
                    pun.i[1] ^= ntohl(node->addr.ia.sin_addr.s_addr);
                free(cur);
            }
        }

        // i[2] process on host
#if defined(_WIN32)
        pun.i[2] = GetCurrentProcessId();
#elif !defined(__rtems__) && !defined(vxWorks)
        pun.i[2] = getpid();
#else
        pun.i[2] = 0xdeadbeef;
#endif
        // and a bit of server instance within this process
        pun.i[2] ^= uint32_t(effective.tcp_port)<<16u;
        // maybe a little bit of randomness (eg. ASLR on Linux)
        pun.i[2] ^= size_t(this);
        if(sizeof(size_t)>4)
            pun.i[2] ^= size_t(this)>>32u;

        std::copy(pun.b.begin(), pun.b.end(), effective.guid.begin());
    }

    // Add magic "server" PV
    {
        auto L = sourcesLock.lockWriter();
        sources[std::make_pair(-1, "server")] = std::make_shared<ServerSource>(this);
    }
}

Server::Pvt::~Pvt()
{
    stop();
}

void Server::Pvt::start()
{
    log_printf(serversetup, Debug, "Server Starting\n%s", "");

    // begin accepting connections
    state_t prev_state;
    acceptor_loop.call([this, &prev_state]()
    {
        prev_state = state;
        if(state!=Stopped) {
            // already running
            log_printf(serversetup, Debug, "Server not stopped %d\n", state);
            return;
        }
        state = Starting;
        log_printf(serversetup, Debug, "Server starting\n%s", "");

        for(auto& iface : interfaces) {
            if(evconnlistener_enable(iface.listener.get())) {
                log_printf(serversetup, Err, "Error enabling listener on %s\n", iface.name.c_str());
            }
            log_printf(serversetup, Debug, "Server enabled listener on %s\n", iface.name.c_str());
        }
    });
    if(prev_state!=Stopped)
        return;

    // being processing Searches
    for(auto& L : listeners) {
        L->start();
    }

    // begin sending beacons
    acceptor_loop.call([this]()
    {
        // send first beacon immediately
        if(event_add(beaconTimer.get(), nullptr))
            log_printf(serversetup, Err, "Error enabling beacon timer on\n%s", "");

        state = Running;
    });


}

void Server::Pvt::stop()
{
    log_printf(serversetup, Debug, "Server Stopping\n%s", "");

    // Stop sending Beacons
    state_t prev_state;
    acceptor_loop.call([this, &prev_state]()
    {
        prev_state = state;
        if(state!=Running) {
            log_printf(serversetup, Debug, "Server not running %d\n", state);
            return;
        }
        state = Stopping;

        if(event_del(beaconTimer.get()))
            log_printf(serversetup, Err, "Error disabling beacon timer on\n%s", "");
    });
    if(prev_state!=Running)
        return;

    // stop processing Search requests
    for(auto& L : listeners) {
        L->stop();
    }

    // stop accepting new TCP connections
    acceptor_loop.call([this]()
    {
        for(auto& iface : interfaces) {
            if(evconnlistener_disable(iface.listener.get())) {
                log_printf(serversetup, Err, "Error disabling listener on %s\n", iface.name.c_str());
            }
            log_printf(serversetup, Debug, "Server disabled listener on %s\n", iface.name.c_str());
        }

        state = Stopped;
    });
}

void Server::Pvt::onSearch(const UDPManager::Search& msg)
{
    // on UDPManager worker

    log_printf(serverio, Debug, "%s searching\n", msg.src.tostring().c_str());

    searchOp._names.resize(msg.names.size());
    for(auto i : range(msg.names.size())) {
        searchOp._names[i]._name = msg.names[i].name;
        searchOp._names[i]._claim = false;
    }

    {
        auto G(sourcesLock.lockReader());
        for(const auto& pair : sources) {
            try {
                pair.second->onSearch(searchOp);
            }catch(std::exception& e){
                log_printf(serversetup, Err, "Unhandled error in Source::onSearch for '%s' : %s\n",
                           pair.first.second.c_str(), e.what());
            }
        }
    }

    uint16_t nreply = 0;
    for(const auto& name : searchOp._names) {
        if(name._claim)
            nreply++;
    }

    // "pvlist" breaks unless we honor mustReply flag
    if(nreply==0 && !msg.mustReply)
        return;

    VectorOutBuf M(true, searchReply);

    M.skip(8); // fill in header after body length known

    _to_wire<12>(M, effective.guid.data(), false);
    to_wire(M, msg.searchID);
    to_wire(M, SockAddr::any(AF_INET));
    to_wire(M, uint16_t(effective.tcp_port));
    to_wire(M, "tcp");
    // "found" flag
    to_wire(M, uint8_t(nreply!=0 ? 1 : 0));

    to_wire(M, uint16_t(nreply));
    for(auto i : range(msg.names.size())) {
        if(searchOp._names[i]._claim)
            to_wire(M, uint32_t(msg.names[i].id));
    }
    auto pktlen = M.save()-searchReply.data();

    // now going back to fill in header
    FixedBuf H(true, searchReply.data(), 8);
    to_wire(H, Header{CMD_SEARCH_RESPONSE, pva_flags::Server, uint32_t(pktlen-8)});

    if(!M.good() || !H.good()) {
        log_printf(serverio, Crit, "Logic error in Search buffer fill\n%s", "");
    } else {
        (void)msg.reply(searchReply.data(), pktlen);
    }
}

void Server::Pvt::doBeacons(short evt)
{
    log_printf(serversetup, Debug, "Server beacon timer expires\n%s", "");

    VectorOutBuf M(true, beaconMsg);
    M.skip(8); // fill in header after body length known

    _to_wire<12>(M, effective.guid.data(), false);
    M.skip(4); // ignored/unused

    to_wire(M, SockAddr::any(AF_INET));
    to_wire(M, uint16_t(effective.tcp_port));
    to_wire(M, "tcp");
    // "NULL" serverStatus
    to_wire(M, uint8_t(0xff));

    auto pktlen = M.save()-searchReply.data();

    // now going back to fill in header
    FixedBuf H(true, searchReply.data(), 8);
    to_wire(H, Header{CMD_BEACON, pva_flags::Server, uint32_t(pktlen-8)});

    assert(M.good() && H.good());

    for(const auto& dest : beaconDest) {
        int ntx = sendto(beaconSender.sock, (char*)beaconMsg.data(), pktlen, 0, &dest->sa, dest.size());

        if(ntx<0) {
            int err = evutil_socket_geterror(beaconSender.sock);
            log_printf(serverio, Warn, "Beacon tx error (%d) %s\n",
                       err, evutil_socket_error_to_string(err));

        } else if(unsigned(ntx)<beaconMsg.size()) {
            log_printf(serverio, Warn, "Beacon truncated %u", unsigned(dest.size()));
        }
    }

    timeval interval = {15, 0};
    if(event_add(beaconTimer.get(), &interval))
        log_printf(serversetup, Err, "Error re-enabling beacon timer on\n%s", "");
}

void Server::Pvt::doBeaconsS(evutil_socket_t fd, short evt, void *raw)
{
    try {
        static_cast<Pvt*>(raw)->doBeacons(evt);
    }catch(std::exception& e){
        log_printf(serverio, Crit, "Unhandled error in beacon timer callback: %s\n", e.what());
    }
}

Source::~Source() {}

Source::List Source::onList() {
    return Source::List{};
}

OpBase::~OpBase() {}

ChannelControl::~ChannelControl() {}

ConnectOp::~ConnectOp() {}
ExecOp::~ExecOp() {}

MonitorControlOp::~MonitorControlOp() {}
MonitorSetupOp::~MonitorSetupOp() {}

}} // namespace pvxs::server
