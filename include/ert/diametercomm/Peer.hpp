/*
 ___________________________________________________________________________
|                                                                           |
|      _ _                      _                                           |
|   __| (_) __ _ _ __ ___   ___| |_ ___ _ __ ___ ___  _ __ ___  _ __ ___    |
|  / _` | |/ _` | '_ ` _ \ / _ \ __/ _ \ '__/ __/ _ \| '_ ` _ \| '_ ` _ \   |
| | (_| | | (_| | | | | | |  __/ ||  __/ | | (_| (_) | | | | | | | | | | |  |
|  \__,_|_|\__,_|_| |_| |_|\___|\__\___|_|  \___\___/|_| |_| |_|_| |_| |_|  |
|                                                                           |
|___________________________________________________________________________|

C++ DIAMETER COMMUNICATIONS LIBRARY
https://github.com/testillano/diametercomm

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2024 Eduardo Ramos
*/

/**
 * @file Peer.hpp
 * @brief Diameter peer state machine with automatic base protocol handling.
 *
 * Manages the lifecycle of a Diameter peer connection:
 * - CER/CEA capability exchange (connection establishment)
 * - DWR/DWA device watchdog (keepalive)
 * - DPR/DPA disconnect peer (graceful shutdown)
 * - Hop-by-hop and end-to-end identifier generation
 * - Application message dispatch via callback
 *
 * The Peer owns a PeerConnection and handles base protocol messages (command
 * codes 257, 280, 282) automatically. Non-base messages are delivered to the
 * application through the RequestCallback.
 *
 * @see PeerConnection for the transport layer
 * @see DiameterServer for multi-peer server management
 * @see DiameterClient for client-side correlation and reconnection
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include <ert/diametercomm/PeerConnection.hpp>

namespace ert
{
namespace diametercomm
{

/**
 * Diameter peer state machine.
 *
 * Manages the lifecycle of a Diameter peer connection including:
 * - CER/CEA capability exchange (connection establishment)
 * - DWR/DWA device watchdog (keepalive)
 * - DPR/DPA disconnect peer (graceful shutdown)
 * - Hop-by-hop / end-to-end ID generation
 *
 * The Peer owns a PeerConnection and handles base protocol messages
 * automatically. Application-level messages (non-base) are delivered
 * to the onRequest callback.
 *
 * RFC 6733 base protocol command codes:
 *   CER/CEA = 257 (Capabilities-Exchange)
 *   DWR/DWA = 280 (Device-Watchdog)
 *   DPR/DPA = 282 (Disconnect-Peer)
 */
class Peer : public std::enable_shared_from_this<Peer> {
public:
    using Buffer = std::vector<uint8_t>;

    enum class State {
        Closed,     // No connection or connection lost
        WaitCEA,    // Client: CER sent, waiting for CEA
        WaitCER,    // Server: accepted connection, waiting for CER
        Open,       // Capability exchange done, ready for traffic
        Closing     // DPR sent, waiting for DPA
    };

    /**
     * Configuration for the peer.
     */
    struct Config {
        std::string originHost;              // Origin-Host for CER/CEA
        std::string originRealm;             // Origin-Realm for CER/CEA
        std::string hostIpAddress{"0.0.0.0"};// Host-IP-Address for CER/CEA
        uint32_t vendorId{0};                // Vendor-Id for CER/CEA
        std::string productName{"h2diagent"};// Product-Name for CER/CEA
        uint32_t watchdogIntervalSec{30};    // DWR interval (0 = disabled)
        uint32_t applicationId{0};           // Supported application
    };

    /**
     * Callback for application-level messages (non-base protocol).
     * The peer has already validated the message and it's in Open state.
     */
    using RequestCallback = std::function<void(std::shared_ptr<Peer>, Buffer&&)>;

    /**
     * Callback for state changes.
     */
    using StateCallback = std::function<void(std::shared_ptr<Peer>, State)>;

    /**
     * Create a client-side peer (will send CER after connect).
     */
    Peer(boost::asio::io_context& io, const Config& config);

    /**
     * Create a server-side peer (will wait for CER after accept).
     */
    Peer(std::shared_ptr<PeerConnection> connection,
         boost::asio::io_context& io, const Config& config);

    ~Peer();

    // Non-copyable
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    /**
     * Client-side: connect to remote peer and initiate CER/CEA.
     */
    void connect(const std::string& host, uint16_t port);

    /**
     * Server-side: start the peer (begin reading, wait for CER).
     */
    void start();

    /**
     * Send an application-level Diameter message.
     * Automatically sets hop-by-hop and end-to-end IDs if they are zero.
     * Returns false if peer is not in Open state.
     */
    bool send(Buffer msg);

    /**
     * Initiate graceful disconnect (send DPR, wait for DPA).
     */
    void disconnect(uint32_t disconnectCause = 0);

    /**
     * Force close (no DPR/DPA exchange).
     */
    void close();

    // --- Accessors ---
    State state() const { return state_; }
    const std::string& remoteOriginHost() const { return remoteOriginHost_; }
    const std::string& remoteOriginRealm() const { return remoteOriginRealm_; }
    uint32_t nextHopByHop() { return hopByHop_.fetch_add(1); }
    uint32_t nextEndToEnd() { return endToEnd_.fetch_add(1); }

    // --- Callbacks ---
    void setRequestCallback(RequestCallback cb) { onRequest_ = std::move(cb); }
    void setStateCallback(StateCallback cb) { onState_ = std::move(cb); }

    // --- Access underlying socket (for acceptor usage in server mode) ---
    boost::asio::ip::tcp::socket& socket() { return connection_->socket(); }

private:

    // Message handling
    void onMessage(Buffer&& msg);
    void onError(const boost::system::error_code& ec);

    // Base protocol handlers
    void handleCER(const Buffer& msg);
    void handleCEA(const Buffer& msg);
    void handleDWR(const Buffer& msg);
    void handleDWA(const Buffer& msg);
    void handleDPR(const Buffer& msg);
    void handleDPA(const Buffer& msg);

    // Base protocol message builders
    Buffer buildCER() const;
    Buffer buildCEA(const Buffer& cer) const;
    Buffer buildDWR();
    Buffer buildDWA(const Buffer& dwr) const;
    Buffer buildDPA(const Buffer& dpr) const;

    // Watchdog timer
    void startWatchdog();
    void stopWatchdog();

    // State management
    void setState(State newState);

    // Header helpers
    static uint32_t extractCommandCode(const Buffer& msg);
    static bool isRequest(const Buffer& msg);
    static uint32_t extractHopByHop(const Buffer& msg);
    static uint32_t extractEndToEnd(const Buffer& msg);
    static void setHopByHop(Buffer& msg, uint32_t hbh);
    static void setEndToEnd(Buffer& msg, uint32_t e2e);

    // Members
    boost::asio::io_context& io_;
    std::shared_ptr<PeerConnection> connection_;
    Config config_;
    State state_{State::Closed};

    std::atomic<uint32_t> hopByHop_{1};
    std::atomic<uint32_t> endToEnd_{1};

    boost::asio::steady_timer watchdogTimer_;

    std::string remoteOriginHost_;
    std::string remoteOriginRealm_;

    RequestCallback onRequest_;
    StateCallback onState_;
};

} // namespace diametercomm
} // namespace ert
