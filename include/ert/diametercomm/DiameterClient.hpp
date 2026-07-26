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
 * @file DiameterClient.hpp
 * @brief Diameter client: connects to a remote peer and manages request/response correlation.
 *
 * Features:
 * - Automatic CER/CEA establishment via underlying Peer
 * - Request/response correlation using hop-by-hop identifier
 * - Per-transaction timeout with configurable duration
 * - Exponential backoff reconnection on connection loss
 * - Unsolicited request handling (server-initiated messages)
 *
 * Usage example:
 * @code
 * DiameterClient client(io, config);
 * client.connect("pcrf.example.com", 3868);
 * // Once ready:
 * client.send(request, [](const Buffer& answer) {
 *     // Process answer
 * }, 5000); // 5s timeout
 * @endcode
 *
 * @see Peer for the underlying state machine
 * @see DiameterServer for the server-side counterpart
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include <ert/diametercomm/Peer.hpp>

namespace ert
{
namespace diametercomm
{

/**
 * Diameter client: connects to a remote peer and sends requests.
 *
 * Manages:
 * - Connection establishment (CER/CEA)
 * - Request/response correlation via hop-by-hop ID
 * - Response timeout per transaction
 * - Automatic reconnection with exponential backoff
 * - Unsolicited request handling (server-initiated messages on the connection)
 *
 * Usage:
 *   DiameterClient client(io, config);
 *   client.connect("server.example.com", 3868);
 *   client.send(request, [](const Buffer& answer) { ... }, timeout);
 */
class DiameterClient {
public:
    using Buffer = Peer::Buffer;
    using ResponseCallback = std::function<void(const Buffer& response)>;
    using TimeoutCallback = std::function<void(uint32_t hopByHop)>;
    using RequestCallback = std::function<void(std::shared_ptr<Peer>, Buffer&&)>;
    using StateCallback = std::function<void(Peer::State)>;

    /**
     * @param io       io_context for async operations
     * @param config   Peer configuration
     */
    DiameterClient(boost::asio::io_context& io, const Peer::Config& config);

    ~DiameterClient();

    // Non-copyable
    DiameterClient(const DiameterClient&) = delete;
    DiameterClient& operator=(const DiameterClient&) = delete;

    /**
     * Connect to a remote Diameter peer.
     * CER/CEA is handled automatically. Once Open, requests can be sent.
     *
     * @param host  Remote host (DNS name or IP)
     * @param port  Remote port (default 3868)
     */
    void connect(const std::string& host, uint16_t port = 3868);

    /**
     * Send a Diameter request and register a callback for the response.
     * Hop-by-hop is auto-assigned if zero.
     *
     * @param request   Complete Diameter request message
     * @param onResponse Called when the correlated answer arrives
     * @param timeoutMs  Timeout in milliseconds (0 = no timeout)
     * @return hop-by-hop ID used, or 0 if send failed (not connected)
     */
    uint32_t send(Buffer request, ResponseCallback onResponse,
                  uint32_t timeoutMs = 5000);

    /**
     * Graceful disconnect (DPR/DPA).
     */
    void disconnect(uint32_t cause = 0);

    /**
     * Force close.
     */
    void close();

    /**
     * Check if the peer is in Open state (ready to send).
     */
    bool isReady() const;

    /**
     * Get current peer state.
     */
    Peer::State state() const;

    // --- Callbacks ---
    /** Called for unsolicited requests (server-initiated on this connection) */
    void setRequestCallback(RequestCallback cb) { onRequest_ = std::move(cb); }
    /** Called on state changes */
    void setStateCallback(StateCallback cb) { onState_ = std::move(cb); }
    /** Called when a response times out */
    void setTimeoutCallback(TimeoutCallback cb) { onTimeout_ = std::move(cb); }

    // --- Reconnect configuration ---
    void setReconnectEnabled(bool enabled) { reconnectEnabled_ = enabled; }
    void setReconnectBackoff(std::chrono::milliseconds initial,
                             std::chrono::milliseconds max) {
        reconnectInitial_ = initial;
        reconnectMax_ = max;
    }

private:

    void onPeerState(std::shared_ptr<Peer> peer, Peer::State state);
    void onPeerRequest(std::shared_ptr<Peer> peer, Buffer&& msg);
    void scheduleReconnect();

    struct PendingRequest {
        ResponseCallback callback;
        boost::asio::steady_timer timer;
        PendingRequest(boost::asio::io_context& io) : timer(io) {}
    };

    boost::asio::io_context& io_;
    Peer::Config config_;
    std::shared_ptr<Peer> peer_;

    std::string host_;
    uint16_t port_{3868};

    // Correlation map: hop-by-hop -> pending request
    mutable std::mutex pendingMutex_;
    std::unordered_map<uint32_t, std::shared_ptr<PendingRequest>> pending_;

    // Reconnect state
    bool reconnectEnabled_{true};
    std::chrono::milliseconds reconnectInitial_{1000};
    std::chrono::milliseconds reconnectMax_{30000};
    std::chrono::milliseconds reconnectCurrent_{1000};
    boost::asio::steady_timer reconnectTimer_;

    RequestCallback onRequest_;
    StateCallback onState_;
    TimeoutCallback onTimeout_;
};

} // namespace diametercomm
} // namespace ert
