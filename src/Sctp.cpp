/*
C++ DIAMETER COMMUNICATIONS LIBRARY
https://github.com/testillano/diametercomm
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <ert/diametercomm/Sctp.hpp>

namespace ert {
namespace diametercomm {
namespace sctp {

namespace {

// sockaddr of an address and port; returns its length.
socklen_t toSockaddr(const boost::asio::ip::address& a, uint16_t port, sockaddr_storage& ss) {
    std::memset(&ss, 0, sizeof(ss));
    if (a.is_v6()) {
        auto* s6 = reinterpret_cast<sockaddr_in6*>(&ss);
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(port);
        const auto bytes = a.to_v6().to_bytes();
        std::memcpy(&s6->sin6_addr, bytes.data(), bytes.size());
        s6->sin6_scope_id = a.to_v6().scope_id();
        return sizeof(sockaddr_in6);
    }
    auto* s4 = reinterpret_cast<sockaddr_in*>(&ss);
    s4->sin_family = AF_INET;
    s4->sin_port = htons(port);
    const auto bytes = a.to_v4().to_bytes();
    std::memcpy(&s4->sin_addr, bytes.data(), bytes.size());
    return sizeof(sockaddr_in);
}

// Packed sockaddr array (the format of the bindx / connectx socket options).
std::vector<uint8_t> packed(const std::vector<boost::asio::ip::address>& addresses, uint16_t port) {
    std::vector<uint8_t> buf;
    for (const auto& a : addresses) {
        sockaddr_storage ss;
        const socklen_t len = toSockaddr(a, port, ss);
        const auto* p = reinterpret_cast<const uint8_t*>(&ss);
        buf.insert(buf.end(), p, p + len);
    }
    return buf;
}

// Address text of a sockaddr (no port); empty for an unknown family.
std::string addressOf(const sockaddr* sa) {
    char text[INET6_ADDRSTRLEN] = {0};
    if (sa->sa_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, text, sizeof(text));
    } else if (sa->sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, text, sizeof(text));
    }
    return text;
}

// Addresses of SCTP_GET_PEER_ADDRS / SCTP_GET_LOCAL_ADDRS, as sockaddr_storage.
std::vector<sockaddr_storage> getAddrs(int fd, int option) {
    std::vector<sockaddr_storage> result;
    std::vector<uint8_t> buf(sizeof(sctp_getaddrs) + 64 * sizeof(sockaddr_in6), 0);
    auto* ga = reinterpret_cast<sctp_getaddrs*>(buf.data());
    ga->assoc_id = 0;  // one-to-one socket: its only association
    socklen_t len = buf.size();
    if (::getsockopt(fd, SOL_SCTP, option, ga, &len) < 0) return result;
    // Walk addr_num addresses within the buffer: the returned length is not
    // reliable for this (Linux counts only the address bytes for the local
    // addresses, without the struct header, which would cut the last ones).
    const uint8_t* p = ga->addrs;
    const uint8_t* end = buf.data() + buf.size();
    for (uint32_t i = 0; i < ga->addr_num && p < end; ++i) {
        const auto* sa = reinterpret_cast<const sockaddr*>(p);
        const std::size_t size = sa->sa_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
        if (p + size > end) break;
        sockaddr_storage ss;
        std::memset(&ss, 0, sizeof(ss));
        std::memcpy(&ss, p, size);
        result.push_back(ss);
        p += size;
    }
    return result;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

}  // namespace

std::string parseAddressList(const std::string& csv, std::vector<boost::asio::ip::address>& out) {
    out.clear();
    std::size_t pos = 0;
    while (pos <= csv.size()) {
        std::size_t comma = csv.find(',', pos);
        if (comma == std::string::npos) comma = csv.size();
        const std::string item = trim(csv.substr(pos, comma - pos));
        if (item.empty()) return "empty address in '" + csv + "'";
        boost::system::error_code ec;
        const auto a = boost::asio::ip::make_address(item, ec);
        if (ec) return "invalid address '" + item + "'";
        if (!out.empty() && a.is_v6() != out.front().is_v6()) return "mixed IPv4 and IPv6 addresses in '" + csv + "'";
        out.push_back(a);
        pos = comma + 1;
    }
    return "";
}

std::string bindAll(int fd, const std::vector<boost::asio::ip::address>& addresses, uint16_t port) {
    if (addresses.empty()) return "no address to bind";
    sockaddr_storage ss;
    const socklen_t len = toSockaddr(addresses.front(), port, ss);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), len) < 0)
        return "bind " + addresses.front().to_string() + ": " + std::strerror(errno);
    if (addresses.size() > 1) {
        std::vector<boost::asio::ip::address> rest(addresses.begin() + 1, addresses.end());
        // the added addresses take the port of the first bind (an ephemeral one included)
        sockaddr_storage bound;
        socklen_t boundLen = sizeof(bound);
        uint16_t effective = port;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
            effective = ntohs(bound.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port
                                                          : reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
        }
        auto buf = packed(rest, effective);
        if (::setsockopt(fd, SOL_SCTP, SCTP_SOCKOPT_BINDX_ADD, buf.data(), buf.size()) < 0)
            return std::string("bindx: ") + std::strerror(errno);
    }
    return "";
}

int connectAll(int fd, const std::vector<boost::asio::ip::address>& peers, uint16_t port) {
    auto buf = packed(peers, port);
    if (::setsockopt(fd, SOL_SCTP, SCTP_SOCKOPT_CONNECTX, buf.data(), buf.size()) < 0) {
        return errno == EINPROGRESS ? 0 : errno;
    }
    return 0;
}

std::string applyOptions(int fd, const SctpOptions& options) {
    if (options.heartbeatIntervalMs == 0 && options.pathMaxRetrans == 0) return "";
    sctp_paddrparams params;
    std::memset(&params, 0, sizeof(params));
    params.spp_assoc_id = 0;  // endpoint defaults: the association(s) to come
    if (options.heartbeatIntervalMs > 0) {
        params.spp_hbinterval = options.heartbeatIntervalMs;
        params.spp_flags |= SPP_HB_ENABLE;
    }
    if (options.pathMaxRetrans > 0) params.spp_pathmaxrxt = options.pathMaxRetrans;
    if (::setsockopt(fd, SOL_SCTP, SCTP_PEER_ADDR_PARAMS, &params, sizeof(params)) < 0)
        return std::string("SCTP_PEER_ADDR_PARAMS: ") + std::strerror(errno);
    return "";
}

std::string setPrimary(int fd, const boost::asio::ip::address& peer, uint16_t port) {
    // The kernel structs are packed: fill a local sockaddr_storage and copy it in
    // (a reference to a packed member cannot be bound).
    sockaddr_storage addr;
    toSockaddr(peer, port, addr);
    sctp_prim prim;
    std::memset(&prim, 0, sizeof(prim));
    prim.ssp_assoc_id = 0;
    std::memcpy(&prim.ssp_addr, &addr, sizeof(addr));
    if (::setsockopt(fd, SOL_SCTP, SCTP_PRIMARY_ADDR, &prim, sizeof(prim)) < 0)
        return "SCTP_PRIMARY_ADDR " + peer.to_string() + ": " + std::strerror(errno);
    return "";
}

std::string stateName(int state) {
    switch (state) {
        case SCTP_ACTIVE:
            return "active";
        case SCTP_INACTIVE:
            return "inactive";
        case SCTP_PF:
            return "potentiallyFailed";
        case SCTP_UNCONFIRMED:
            return "unconfirmed";
        default:
            return "unknown";
    }
}

std::vector<PathInfo> paths(int fd) {
    std::vector<PathInfo> result;
    if (fd < 0) return result;

    std::string primary;
    sctp_prim prim;
    std::memset(&prim, 0, sizeof(prim));
    socklen_t primLen = sizeof(prim);
    if (::getsockopt(fd, SOL_SCTP, SCTP_PRIMARY_ADDR, &prim, &primLen) == 0) {
        sockaddr_storage addr;
        std::memcpy(&addr, &prim.ssp_addr, sizeof(addr));  // packed member: copied out
        primary = addressOf(reinterpret_cast<const sockaddr*>(&addr));
    }

    for (const auto& ss : getAddrs(fd, SCTP_GET_PEER_ADDRS)) {
        PathInfo path;
        path.address = addressOf(reinterpret_cast<const sockaddr*>(&ss));
        sctp_paddrinfo info;
        std::memset(&info, 0, sizeof(info));
        info.spinfo_assoc_id = 0;
        std::memcpy(&info.spinfo_address, &ss, sizeof(ss));
        socklen_t infoLen = sizeof(info);
        path.state = ::getsockopt(fd, SOL_SCTP, SCTP_GET_PEER_ADDR_INFO, &info, &infoLen) == 0
                         ? stateName(info.spinfo_state)
                         : "unknown";
        path.primary = (!primary.empty() && primary == path.address);
        result.push_back(path);
    }
    return result;
}

std::vector<std::string> localAddresses(int fd) {
    std::vector<std::string> result;
    if (fd < 0) return result;
    for (const auto& ss : getAddrs(fd, SCTP_GET_LOCAL_ADDRS))
        result.push_back(addressOf(reinterpret_cast<const sockaddr*>(&ss)));
    return result;
}

}  // namespace sctp
}  // namespace diametercomm
}  // namespace ert
