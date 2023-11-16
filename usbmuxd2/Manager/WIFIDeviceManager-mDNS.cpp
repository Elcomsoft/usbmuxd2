//
//  WIFIDeviceManager-mDNS.cpp
//  usbmuxd2
//
//  Created by tihmstar on 26.09.19.
//  Copyright © 2019 tihmstar. All rights reserved.
//


#include <libgeneral/macros.h>

#include "WIFIDeviceManager-mDNS.hpp"
#include "../Devices/WIFIDevice.hpp"
#include "../sysconf/sysconf.hpp"
#include "../Devices/WIFIDevice.hpp"
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>


#ifdef HAVE_WIFI_MDNS

#pragma mark definitions

#define kDNSServiceInterfaceIndexAny 0
#define kDNSServiceInterfaceIndexLocalOnly ((uint32_t)-1)
#define kDNSServiceInterfaceIndexUnicast   ((uint32_t)-2)
#define kDNSServiceInterfaceIndexP2P       ((uint32_t)-3)
#define kDNSServiceInterfaceIndexBLE       ((uint32_t)-4)


#define kDNSServiceFlagsMoreComing 0x1
#define kDNSServiceFlagsAdd     0x2
#define LONG_TIME 100000000

#define kDNSServiceProtocol_IPv4 0x01
#define kDNSServiceProtocol_IPv6 0x02


#pragma mark callbacks
void getaddr_reply(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex, DNSServiceErrorType errorCode, const char *hostname, const struct sockaddr *address, uint32_t ttl, void *context) noexcept{
    int err = 0;
    WIFIDeviceManager *devmgr = (WIFIDeviceManager *)context;
    
    std::vector<std::string> &addrs = devmgr->_clientAddrs[sdRef];

    std::string ipaddr;
    ipaddr.resize(INET6_ADDRSTRLEN+1);
    if (address->sa_family == AF_INET6) {
        ipaddr = inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)address)->sin6_addr), ipaddr.data(), (socklen_t)ipaddr.size());
    }else{
        ipaddr = inet_ntop(AF_INET, &(((struct sockaddr_in *)address)->sin_addr), ipaddr.data(), (socklen_t)ipaddr.size());
    }
    addrs.push_back(ipaddr);


    if (!(flags & kDNSServiceFlagsMoreComing)) {
        std::string serviceName = addrs.front();
        addrs.erase(addrs.begin());
        
        std::string macAddr{serviceName.substr(0,serviceName.find("@"))};
        std::string uuid;
        try{
            uuid = sysconf_udid_for_macaddr(macAddr);
        }catch (tihmstar::exception &e){
            creterror("failed to find uuid for mac=%s with error=%d (%s)",macAddr.c_str(),e.code(),e.what());
        }
    
        if (!devmgr->_mux->have_wifi_device(macAddr)) {
            std::shared_ptr<WIFIDevice> dev = nullptr;
            try{
                dev = std::make_shared<WIFIDevice>(devmgr->_mux, uuid, addrs, serviceName);
                devmgr->device_add(dev); dev = NULL;
            } catch (tihmstar::exception &e){
                creterror("failed to construct device with error=%d (%s)",e.code(),e.what());
            }
        }
    }
    
error:
    if (!(flags & kDNSServiceFlagsMoreComing)) {
        devmgr->_clientAddrs.erase(sdRef);
        DNSServiceRef sdResolv = devmgr->_linkedClients[sdRef];
        devmgr->_linkedClients.erase(sdRef);
        devmgr->_removeClients.push_back(sdRef); //idk why, but order is important!
        devmgr->_removeClients.push_back(sdResolv);
    }
    if (err) {
        error("getaddr_reply failed with error=%d",err);
    }
}

void resolve_reply(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex, DNSServiceErrorType errorCode, const char *fullname, const char *hosttarget, uint16_t port, uint16_t txtLen, const unsigned char *txtRecord, void *context) noexcept{
    int err = 0;
    WIFIDeviceManager *devmgr = (WIFIDeviceManager *)context;
    DNSServiceErrorType res = 0;
    DNSServiceRef resolvClient = NULL;
    int resolvfd = -1;

    cassure(!(res = DNSServiceGetAddrInfo(&resolvClient, 0, kDNSServiceInterfaceIndexAny, kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6, hosttarget, getaddr_reply, context)));
    
    cassure((resolvfd = DNSServiceRefSockFD(resolvClient))>0);
    devmgr->_pfds.push_back({
        .fd = resolvfd,
        .events = POLLIN
    });

    devmgr->_clientAddrs[resolvClient] = {fullname};
    
    devmgr->_resolveClients.push_back(resolvClient);
    devmgr->_linkedClients[resolvClient] = sdRef;
    
error:
    if (err) {
        error("resolve_reply failed with error=%d",err);
    }
}

void browse_reply(DNSServiceRef sdref, const DNSServiceFlags flags, uint32_t ifIndex, DNSServiceErrorType errorCode, const char *replyName, const char *replyType, const char *replyDomain, void *context) noexcept{
    int err = 0;
    DNSServiceErrorType res = 0;
    WIFIDeviceManager *devmgr = (WIFIDeviceManager *)context;
    DNSServiceRef resolvClient = NULL;
    int resolvfd = -1;

    if (!(flags & kDNSServiceFlagsAdd)) {
        debug("ignoring event=%d. We only care about Add events at the moment",flags);
        return;
    }

    const char *op = (flags & kDNSServiceFlagsAdd) ? "Add" : "Rmv";
        printf("%s %8X %3d %-20s %-20s %s\n",
               op, flags, ifIndex, replyDomain, replyType, replyName);

    cassure(!(res = DNSServiceResolve(&resolvClient, 0, kDNSServiceInterfaceIndexAny, replyName, replyType, replyDomain, resolve_reply, context)));

    cassure((resolvfd = DNSServiceRefSockFD(resolvClient))>0);
    devmgr->_pfds.push_back({
        .fd = resolvfd,
        .events = POLLIN
    });

    devmgr->_resolveClients.push_back(resolvClient);

error:
    if (err) {
        error("browse_reply failed with error=%d",err);
    }
}


#pragma mark WIFIDevice

WIFIDeviceManager::WIFIDeviceManager(Muxer *mux)
: DeviceManager(mux), _client(NULL), _dns_sd_fd(-1)
{
    int err = 0;
    debug("WIFIDeviceManager mDNS-client");
    assure(!(err = DNSServiceBrowse(&_client, 0, kDNSServiceInterfaceIndexAny, "_apple-mobdev2._tcp", "", browse_reply, this)));

    assure((_dns_sd_fd = DNSServiceRefSockFD(_client))>0);

    _pfds.push_back({
        .fd = _dns_sd_fd,
        .events = POLLIN
    });
}

WIFIDeviceManager::~WIFIDeviceManager(){
    safeFreeCustom(_client, DNSServiceRefDeallocate);
}

void WIFIDeviceManager::device_add(std::shared_ptr<WIFIDevice> dev){
    dev->_selfref = dev;
    _mux->add_device(dev);
}

void WIFIDeviceManager::kill() noexcept{
    debug("[WIFIDeviceManager] killing WIFIDeviceManager");
    stopLoop();
}

bool WIFIDeviceManager::loopEvent(){
    int res = 0;
    res = poll(_pfds.data(), (int)_pfds.size(), -1);
    if (res > 0){
        cleanup([&]{
            for (auto &rc : _removeClients) {
                const auto target = std::remove(_resolveClients.begin(), _resolveClients.end(), rc);
                if (target != _resolveClients.end()){
                    DNSServiceRef tgt = *target;
                    _resolveClients.erase(target, _resolveClients.end());
                    int rcfd = DNSServiceRefSockFD(tgt);
                    auto iter = std::find_if(_pfds.begin(), _pfds.end(), [&](const struct pollfd &a){
                        return a.fd == rcfd;
                    });
                    assert(iter != _pfds.end());
                    _pfds.erase(iter);
                    DNSServiceRefDeallocate(tgt);
                }
            }
            _removeClients.clear();
        });
        DNSServiceErrorType err = 0;
        for (auto pfd : _pfds) {
            if (pfd.revents & POLLIN) {
                pfd.revents &= ~POLLIN;
                if (pfd.fd == DNSServiceRefSockFD(_client)) {
                    assure(!(err |= DNSServiceProcessResult(_client)));
                }else{
                    for (auto rc : _resolveClients) {
                        int rcfd = DNSServiceRefSockFD(rc);
                        if (rcfd == pfd.fd) {
                            assure(!(err |= DNSServiceProcessResult(rc)));
                            break;
                        }
                    }
                }
            }
        }
    }else if (res != 0){
        reterror("poll() returned %d errno %d %s\n", res, errno, strerror(errno));
    }
    return true;
}

#endif //HAVE_WIFI_MDNS
