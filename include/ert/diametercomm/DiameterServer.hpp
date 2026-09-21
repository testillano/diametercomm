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

#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <ert/diametercomm/Peer.hpp>
#include <ert/metrics/Metrics.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ert {
namespace diametercomm {

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
    using RequestCallback = std::function<void(std::shared_ptr<Peer>, Peer::Buffer &&)>;
    using PeerEventCallback = std::function<void(std::shared_ptr<Peer>, Peer::State)>;
    // Bidirectional Diameter (RFC 6733): the server may itself initiate a
    // request (e.g. RAR) on a connected peer and correlate the incoming answer.
    using ResponseCallback = std::function<void(const Peer::Buffer &response)>;
    using TimeoutCallback = std::function<void(uint32_t hopByHop)>;

    /**
     * @param io       io_context for async operations
     * @param config   Peer configuration (applied to all accepted peers)
     * @param transport TCP or SCTP (default: TCP)
     */
    DiameterServer(boost::asio::io_context &io, const Peer::Config &config, Transport transport = Transport::TCP);

    ~DiameterServer();

    // Non-copyable
    DiameterServer(const DiameterServer &) = delete;
    DiameterServer &operator=(const DiameterServer &) = delete;

    /**
     * Start listening on the given address and port.
     * Begins accepting connections immediately.
     */
    void listen(const std::string &bindAddress, uint16_t port);

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
    /** Called when a server-initiated request (sendRequest) times out. */
    void setTimeoutCallback(TimeoutCallback cb) { onTimeout_ = std::move(cb); }

    /**
     * Send a server-initiated Diameter request through the given peer and
     * register a callback for the correlated answer (bidirectional Diameter,
     * RFC 6733). Hop-by-hop is auto-assigned if zero. Mirrors
     * DiameterClient::send: it tracks a pending transaction keyed by hop-by-hop,
     * arms a timeout, and (when metrics are enabled) increments
     * diameter_server_requests_sent_counter; the correlated answer increments
     * diameter_server_answers_received_counter.
     *
     * @param peer       The peer to send the request through.
     * @param request    Complete Diameter request message.
     * @param onResponse Called when the correlated answer arrives.
     * @param timeoutMs  Timeout in milliseconds (0 = no timeout).
     * @param additionalLabels Extra metric labels reused for the correlated answer.
     * @return hop-by-hop ID used, or 0 if the send failed (e.g. null peer).
     */
    uint32_t sendRequest(std::shared_ptr<Peer> peer, Peer::Buffer request, ResponseCallback onResponse,
                         uint32_t timeoutMs = 5000, const ert::metrics::labels_t &additionalLabels = {});

    /**
     * Send a Diameter answer through the given peer, incrementing metrics.
     * This is the recommended way to send answers when metrics are enabled.
     * Falls back to peer->send() if metrics are disabled.
     *
     * @param peer   The peer to send the answer through.
     * @param answer Complete Diameter answer message.
     * @return true if send succeeded, false otherwise.
     */
    bool sendAnswer(std::shared_ptr<Peer> peer, Peer::Buffer answer);

    // --- Metrics ---

    /**
     * Enable prometheus metrics for the diameter server.
     * Must be called before listen(). Metrics are optional -- if not enabled,
     * no overhead is incurred (nullptr checks guard all metric operations).
     *
     * @param metrics  Pointer to the ert::metrics::Metrics instance (registry/exposer).
     * @param source   Label value for the "source" prometheus label. If empty,
     *                 defaults to "diameter_server".
     */
    void enableMetrics(ert::metrics::Metrics *metrics, const std::string &source = "");

   private:
    void doAccept();
    void onPeerStateChange(std::shared_ptr<Peer> peer, Peer::State state);

    // Build the server metric label set: base {source, command_code,
    // application_id} plus result_code (answers only) and any configured extra
    // labels. Mirrors DiameterClient::clientLabels.
    ert::metrics::labels_t serverLabels(const std::string &commandCode, const std::string &applicationId,
                                        const ert::metrics::labels_t &additionalLabels,
                                        const std::string &resultCode = "") const;

    // Pending server-initiated transaction (awaiting the correlated answer).
    struct PendingRequest {
        ResponseCallback callback;
        boost::asio::steady_timer timer;
        std::chrono::steady_clock::time_point sentAt;
        ert::metrics::labels_t additionalLabels;
        PendingRequest(boost::asio::io_context &io) : timer(io) {}
    };

    boost::asio::io_context &io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    Peer::Config config_;
    Transport transport_;
    std::shared_ptr<boost::asio::ssl::context> serverCtx_;  // prebuilt TLS/TCP server context (null if insecure)

    mutable std::mutex peersMutex_;
    std::vector<std::shared_ptr<Peer>> peers_;

    RequestCallback onRequest_;
    PeerEventCallback onPeerEvent_;
    TimeoutCallback onTimeout_;
    bool listening_{false};

    // Correlation map for server-initiated requests: hop-by-hop -> pending.
    mutable std::mutex pendingMutex_;
    std::unordered_map<uint32_t, std::shared_ptr<PendingRequest>> pending_;

    // --- Metrics members ---
    ert::metrics::Metrics *metrics_{};
    std::string source_{};

    ert::metrics::counter_family_t *requests_received_counter_family_ptr_{};
    ert::metrics::counter_family_t *answers_sent_counter_family_ptr_{};
    ert::metrics::counter_family_t *peer_connections_counter_family_ptr_{};
    ert::metrics::gauge_family_t *active_peers_gauge_family_ptr_{};
    // Bidirectional (RFC 6733): server-initiated requests and their answers.
    ert::metrics::counter_family_t *requests_sent_counter_family_ptr_{};
    ert::metrics::counter_family_t *answers_received_counter_family_ptr_{};
};

}  // namespace diametercomm
}  // namespace ert
