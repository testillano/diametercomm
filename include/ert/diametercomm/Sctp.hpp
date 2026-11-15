/*
C++ DIAMETER COMMUNICATIONS LIBRARY
https://github.com/testillano/diametercomm
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

/**
 * @file Sctp.hpp
 * @brief SCTP multihoming helpers (Linux kernel SCTP, one-to-one sockets).
 *
 * Multihoming: an association may use several local and several peer addresses
 * (paths). The kernel SCTP stack supervises every path (heartbeats), selects the
 * primary one and fails over to another when it stops answering: transparent to
 * Diameter. This module only configures it (several local addresses, several
 * peer addresses, heartbeat interval, path max retransmissions, primary path)
 * and reports it (paths and their state), through socket options (no libsctp
 * link dependency).
 *
 * Path state is polled (SCTP_GET_PEER_ADDR_INFO) instead of subscribing to the
 * SCTP_PEER_ADDR_CHANGE notifications: notifications are delivered in-band by
 * recvmsg(MSG_NOTIFICATION), which the stream reads of the connection would take
 * as Diameter bytes.
 */

#pragma once

#include <boost/asio.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace ert {
namespace diametercomm {

/** SCTP association options (ignored for TCP). Zero / empty: kernel defaults. */
struct SctpOptions {
    uint32_t heartbeatIntervalMs{0};  // path heartbeat interval (SCTP_PEER_ADDR_PARAMS spp_hbinterval)
    uint16_t pathMaxRetrans{0};       // retransmissions before a path is inactive (spp_pathmaxrxt)
    std::string primaryPeerAddress;   // client side: peer address to use as primary path (SCTP_PRIMARY_ADDR)
};

namespace sctp {

/** A peer address (path) of an association. */
struct PathInfo {
    std::string address;  // peer address (no port)
    std::string state;    // active | inactive | potentiallyFailed | unconfirmed | unknown
    bool primary{false};
};

/**
 * Parses a comma separated address list ("10.0.0.1,10.0.0.2"; blanks ignored).
 * All the addresses must be of the same family (IPv4 or IPv6).
 * @return empty on success, else the error
 */
std::string parseAddressList(const std::string& csv, std::vector<boost::asio::ip::address>& out);

/** True when the text holds more than one address (a ','). */
inline bool isAddressList(const std::string& text) { return text.find(',') != std::string::npos; }

/**
 * Binds an SCTP socket to every address (the first with bind(), the others
 * added with SCTP_SOCKOPT_BINDX_ADD), all with the same port (0 = ephemeral).
 * @return empty on success, else the error
 */
std::string bindAll(int fd, const std::vector<boost::asio::ip::address>& addresses, uint16_t port);

/**
 * Starts a non blocking connection to every peer address (SCTP_SOCKOPT_CONNECTX).
 * @return 0 when connected or in progress (wait for writability, then SO_ERROR),
 *         else the errno
 */
int connectAll(int fd, const std::vector<boost::asio::ip::address>& peers, uint16_t port);

/** Applies heartbeat interval and path max retransmissions (endpoint defaults). */
std::string applyOptions(int fd, const SctpOptions& options);

/** Sets the primary peer address of the association. */
std::string setPrimary(int fd, const boost::asio::ip::address& peer, uint16_t port);

/** Peer addresses of the association, with their state and the primary one. */
std::vector<PathInfo> paths(int fd);

/** Local addresses of the association (or of the bound socket). */
std::vector<std::string> localAddresses(int fd);

/** State name of a path state value (SCTP_ACTIVE...). */
std::string stateName(int state);

}  // namespace sctp
}  // namespace diametercomm
}  // namespace ert
