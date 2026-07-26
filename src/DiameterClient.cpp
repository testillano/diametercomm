/*
C++ DIAMETER COMMUNICATIONS LIBRARY
https://github.com/testillano/diametercomm
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#include <ert/diametercomm/DiameterClient.hpp>

namespace ert
{
namespace diametercomm
{

// Header helpers (same as Peer, duplicated to avoid exposing private statics)
namespace {

uint32_t extractHopByHop(const Peer::Buffer& msg) {
    return (uint32_t(msg[12]) << 24) | (uint32_t(msg[13]) << 16) |
           (uint32_t(msg[14]) << 8) | uint32_t(msg[15]);
}

void setHopByHop(Peer::Buffer& msg, uint32_t hbh) {
    msg[12] = static_cast<uint8_t>(hbh >> 24);
    msg[13] = static_cast<uint8_t>(hbh >> 16);
    msg[14] = static_cast<uint8_t>(hbh >> 8);
    msg[15] = static_cast<uint8_t>(hbh);
}

bool isRequest(const Peer::Buffer& msg) {
    return (msg[4] & 0x80) != 0;
}

} // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

DiameterClient::DiameterClient(boost::asio::io_context& io, const Peer::Config& config)
    : io_(io)
    , config_(config)
    , reconnectTimer_(io)
{
}

DiameterClient::~DiameterClient() {
    reconnectEnabled_ = false;
    reconnectTimer_.cancel();
    close();
}

// ============================================================================
// connect
// ============================================================================
void DiameterClient::connect(const std::string& host, uint16_t port) {
    host_ = host;
    port_ = port;
    reconnectCurrent_ = reconnectInitial_;

    peer_ = std::make_shared<Peer>(io_, config_);

    peer_->setStateCallback(
        [this](std::shared_ptr<Peer> p, Peer::State s) {
            onPeerState(std::move(p), s);
        });

    peer_->setRequestCallback(
        [this](std::shared_ptr<Peer> p, Buffer&& msg) {
            onPeerRequest(std::move(p), std::move(msg));
        });

    peer_->connect(host_, port_);
}

// ============================================================================
// send
// ============================================================================
uint32_t DiameterClient::send(Buffer request, ResponseCallback onResponse,
                              uint32_t timeoutMs) {
    if (!peer_ || peer_->state() != Peer::State::Open) return 0;
    if (request.size() < 20) return 0;

    // Assign hop-by-hop
    uint32_t hbh = extractHopByHop(request);
    if (hbh == 0) {
        hbh = peer_->nextHopByHop();
        setHopByHop(request, hbh);
    }

    // Register pending request
    auto pending = std::make_shared<PendingRequest>(io_);
    pending->callback = std::move(onResponse);

    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_[hbh] = pending;
    }

    // Set timeout
    if (timeoutMs > 0) {
        pending->timer.expires_after(std::chrono::milliseconds(timeoutMs));
        pending->timer.async_wait(
            [this, hbh](const boost::system::error_code& ec) {
                if (ec) return; // cancelled
                std::shared_ptr<PendingRequest> req;
                {
                    std::lock_guard<std::mutex> lock(pendingMutex_);
                    auto it = pending_.find(hbh);
                    if (it == pending_.end()) return;
                    req = it->second;
                    pending_.erase(it);
                }
                if (onTimeout_) onTimeout_(hbh);
            });
    }

    // Send the request
    peer_->send(std::move(request));
    return hbh;
}

// ============================================================================
// disconnect
// ============================================================================
void DiameterClient::disconnect(uint32_t cause) {
    reconnectEnabled_ = false;
    reconnectTimer_.cancel();
    if (peer_) peer_->disconnect(cause);
}

// ============================================================================
// close
// ============================================================================
void DiameterClient::close() {
    reconnectEnabled_ = false;
    reconnectTimer_.cancel();
    if (peer_) peer_->close();

    // Cancel all pending requests
    std::lock_guard<std::mutex> lock(pendingMutex_);
    for (auto& [hbh, req] : pending_) {
        req->timer.cancel();
    }
    pending_.clear();
}

// ============================================================================
// isReady / state
// ============================================================================
bool DiameterClient::isReady() const {
    return peer_ && peer_->state() == Peer::State::Open;
}

Peer::State DiameterClient::state() const {
    return peer_ ? peer_->state() : Peer::State::Closed;
}

// ============================================================================
// onPeerState
// ============================================================================
void DiameterClient::onPeerState(std::shared_ptr<Peer> /*peer*/, Peer::State state) {
    if (onState_) onState_(state);

    if (state == Peer::State::Open) {
        // Reset reconnect backoff on successful connection
        reconnectCurrent_ = reconnectInitial_;
    } else if (state == Peer::State::Closed) {
        // Connection lost: attempt reconnect
        if (reconnectEnabled_ && !host_.empty()) {
            scheduleReconnect();
        }
    }
}

// ============================================================================
// onPeerRequest - handle answers (correlate) and unsolicited requests
// ============================================================================
void DiameterClient::onPeerRequest(std::shared_ptr<Peer> peer, Buffer&& msg) {
    if (msg.size() < 20) return;

    // If it's an answer (not request), correlate by hop-by-hop
    if (!isRequest(msg)) {
        uint32_t hbh = extractHopByHop(msg);
        std::shared_ptr<PendingRequest> req;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pending_.find(hbh);
            if (it != pending_.end()) {
                req = it->second;
                pending_.erase(it);
            }
        }
        if (req) {
            req->timer.cancel();
            if (req->callback) req->callback(msg);
            return;
        }
    }

    // Unsolicited request from the server side
    if (onRequest_) {
        onRequest_(std::move(peer), std::move(msg));
    }
}

// ============================================================================
// scheduleReconnect
// ============================================================================
void DiameterClient::scheduleReconnect() {
    reconnectTimer_.expires_after(reconnectCurrent_);
    reconnectTimer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return; // cancelled

        // Exponential backoff
        reconnectCurrent_ = std::min(reconnectCurrent_ * 2, reconnectMax_);

        // Reconnect
        connect(host_, port_);
    });
}

} // namespace diametercomm
} // namespace ert
