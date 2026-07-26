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
 * @file DiameterServer.hpp
 * @brief Diameter server: listens for incoming peer connections.
 *
 * Manages the TCP/SCTP acceptor and creates Peer instances for each
 * accepted connection. Features:
 * - Accept loop with automatic Peer creation and CER/CEA handling
 * - Multi-peer tracking (add on Open, remove on Closed)
 * - Unified request callback for all connected peers
 * - Graceful shutdown via DPR/DPA to all active peers
 * - SCTP support via native socket (one-to-one style)
 *
 * @see Peer for individual peer state management
 * @see DiameterClient for the client-side counterpart
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include <ert/diametercomm/Peer.hpp>

namespace ert
{
namespace diametercomm
{

/**
 * Diameter server: listens for incoming peer connections.
 *
 * Manages the TCP acceptor and creates Peer instances for each accepted
 * connection. Handles:
 * - Accept loop (multiple simultaneous peers)
 * - Peer lifecycle tracking (add on CER/CEA, remove on close)
 * - Request dispatch to application via callback
 *
 * The application receives requests from any connected peer through a
 * single callback. The Peer reference is provided so the application
 * can send answers back to the correct peer.
 */
class DiameterServer {
public:

    using RequestCallback = std::function<void(std::shared_ptr<Peer>, Peer::Buffer&&)>;
    using PeerEventCallback = std::function<void(std::shared_ptr<Peer>, Peer::State)>;

    /**
     * @param io       io_context for async operations
     * @param config   Peer configuration (applied to all accepted peers)
     * @param transport TCP or SCTP (default: TCP)
     */
    DiameterServer(boost::asio::io_context& io, const Peer::Config& config,
                   Transport transport = Transport::TCP);

    ~DiameterServer();

    // Non-copyable
    DiameterServer(const DiameterServer&) = delete;
    DiameterServer& operator=(const DiameterServer&) = delete;

    /**
     * Start listening on the given address and port.
     * Begins accepting connections immediately.
     */
    void listen(const std::string& bindAddress, uint16_t port);

    /**
     * Stop accepting new connections.
     * Existing peers remain active until they disconnect or close() is called.
     */
    void stopListening();

    /**
     * Graceful shutdown: send DPR to all peers, wait for DPA, then close.
     */
    void shutdown(uint32_t disconnectCause = 0);

    /**
     * Force close all peers immediately.
     */
    void close();

    /**
     * Get the number of currently active (Open state) peers.
     */
    size_t activePeerCount() const;

    /**
     * Get all active peers.
     */
    std::vector<std::shared_ptr<Peer>> peers() const;

    // --- Callbacks ---
    void setRequestCallback(RequestCallback cb) { onRequest_ = std::move(cb); }
    void setPeerEventCallback(PeerEventCallback cb) { onPeerEvent_ = std::move(cb); }

private:

    void doAccept();
    void onPeerStateChange(std::shared_ptr<Peer> peer, Peer::State state);

    boost::asio::io_context& io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    Peer::Config config_;
    Transport transport_;

    mutable std::mutex peersMutex_;
    std::vector<std::shared_ptr<Peer>> peers_;

    RequestCallback onRequest_;
    PeerEventCallback onPeerEvent_;
    bool listening_{false};
};

} // namespace diametercomm
} // namespace ert
