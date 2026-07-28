/*
C++ DIAMETER COMMUNICATIONS LIBRARY
https://github.com/testillano/diametercomm
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#include <ert/diametercomm/DiameterClient.hpp>
#include <ert/tracing/Logger.hpp>

namespace ert {
namespace diametercomm {

// Header helpers (same as Peer, duplicated to avoid exposing private statics)
namespace {

uint32_t extractHopByHop(const Peer::Buffer& msg) {
    return (uint32_t(msg[12]) << 24) | (uint32_t(msg[13]) << 16) | (uint32_t(msg[14]) << 8) | uint32_t(msg[15]);
}

void setHopByHop(Peer::Buffer& msg, uint32_t hbh) {
    msg[12] = static_cast<uint8_t>(hbh >> 24);
    msg[13] = static_cast<uint8_t>(hbh >> 16);
    msg[14] = static_cast<uint8_t>(hbh >> 8);
    msg[15] = static_cast<uint8_t>(hbh);
}

bool isRequest(const Peer::Buffer& msg) { return (msg[4] & 0x80) != 0; }

uint32_t extractCommandCode(const Peer::Buffer& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[5]) << 16) | (uint32_t(msg[6]) << 8) | uint32_t(msg[7]);
}

uint32_t extractResultCode(const Peer::Buffer& msg) {
    // Result-Code AVP (code 268): walk AVPs to find it.
    if (msg.size() < 20) return 0;

    size_t offset = 20;  // skip diameter header
    while (offset + 8 <= msg.size()) {
        uint32_t avpCode = (uint32_t(msg[offset]) << 24) | (uint32_t(msg[offset + 1]) << 16) |
                           (uint32_t(msg[offset + 2]) << 8) | uint32_t(msg[offset + 3]);
        uint8_t flags = msg[offset + 4];
        uint32_t avpLen =
            (uint32_t(msg[offset + 5]) << 16) | (uint32_t(msg[offset + 6]) << 8) | uint32_t(msg[offset + 7]);
        if (avpLen < 8) break;  // malformed

        size_t headerLen = (flags & 0x80) ? 12 : 8;  // V bit -> vendor-id present
        if (avpCode == 268 && offset + headerLen + 4 <= msg.size()) {
            size_t dataOffset = offset + headerLen;
            return (uint32_t(msg[dataOffset]) << 24) | (uint32_t(msg[dataOffset + 1]) << 16) |
                   (uint32_t(msg[dataOffset + 2]) << 8) | uint32_t(msg[dataOffset + 3]);
        }

        // Advance to next AVP (padded to 4-byte boundary)
        size_t padded = (avpLen + 3) & ~size_t(3);
        offset += padded;
    }

    return 0;  // not found
}

}  // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

DiameterClient::DiameterClient(boost::asio::io_context& io, const Peer::Config& config)
    : io_(io), config_(config), reconnectTimer_(io) {}

DiameterClient::~DiameterClient() {
    reconnectEnabled_ = false;
    reconnectTimer_.cancel();
    close();
}

void DiameterClient::enableMetrics(ert::metrics::Metrics* metrics, const std::string& source) {
    metrics_ = metrics;

    if (metrics_) {
        source_ = source.empty() ? "diameter_client" : source;
        ert::metrics::labels_t familyLabels = {};

        requests_sent_counter_family_ptr_ = &(metrics_->addCounterFamily(
            "diameter_client_requests_sent_counter", "Diameter client requests sent counter", familyLabels));

        answers_received_counter_family_ptr_ = &(metrics_->addCounterFamily(
            "diameter_client_answers_received_counter", "Diameter client answers received counter", familyLabels));

        requests_timedout_counter_family_ptr_ = &(metrics_->addCounterFamily(
            "diameter_client_requests_timedout_counter", "Diameter client requests timed out counter", familyLabels));

        requests_unsent_counter_family_ptr_ = &(metrics_->addCounterFamily(
            "diameter_client_requests_unsent_counter", "Diameter client requests unsent counter", familyLabels));

        peer_state_gauge_family_ptr_ = &(metrics_->addGaugeFamily(
            "diameter_client_peer_state_gauge", "Diameter client peer state (1=open, 0=closed)", familyLabels));
    }
}

// ============================================================================
// connect
// ============================================================================
void DiameterClient::connect(const std::string& host, uint16_t port) {
    host_ = host;
    port_ = port;
    reconnectCurrent_ = reconnectInitial_;

    peer_ = std::make_shared<Peer>(io_, config_);

    peer_->setStateCallback([this](std::shared_ptr<Peer> p, Peer::State s) { onPeerState(std::move(p), s); });

    peer_->setRequestCallback(
        [this](std::shared_ptr<Peer> p, Buffer&& msg) { onPeerRequest(std::move(p), std::move(msg)); });

    peer_->connect(host_, port_);
}

// ============================================================================
// send
// ============================================================================
uint32_t DiameterClient::send(Buffer request, ResponseCallback onResponse, uint32_t timeoutMs) {
    if (!peer_ || peer_->state() != Peer::State::Open) {
        if (metrics_ && request.size() >= 20) {
            std::string commandCode = std::to_string(extractCommandCode(request));
            auto& counter =
                requests_unsent_counter_family_ptr_->Add({{"source", source_}, {"command_code", commandCode}});
            counter.Increment();
        }
        return 0;
    }
    if (request.size() < 20) return 0;

    // Extract command code for metrics before moving the buffer
    std::string commandCode;
    if (metrics_) {
        commandCode = std::to_string(extractCommandCode(request));
    }

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
        pending->timer.async_wait([this, hbh, commandCode](const boost::system::error_code& ec) {
            if (ec) return;  // cancelled
            std::shared_ptr<PendingRequest> req;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                auto it = pending_.find(hbh);
                if (it == pending_.end()) return;
                req = it->second;
                pending_.erase(it);
            }
            if (metrics_) {
                auto& counter =
                    requests_timedout_counter_family_ptr_->Add({{"source", source_}, {"command_code", commandCode}});
                counter.Increment();
            }
            if (onTimeout_) onTimeout_(hbh);
        });
    }

    // Send the request
    peer_->send(std::move(request));

    if (metrics_) {
        auto& counter = requests_sent_counter_family_ptr_->Add({{"source", source_}, {"command_code", commandCode}});
        counter.Increment();
    }

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
bool DiameterClient::isReady() const { return peer_ && peer_->state() == Peer::State::Open; }

Peer::State DiameterClient::state() const { return peer_ ? peer_->state() : Peer::State::Closed; }

// ============================================================================
// onPeerState
// ============================================================================
void DiameterClient::onPeerState(std::shared_ptr<Peer> /*peer*/, Peer::State state) {
    if (onState_) onState_(state);

    if (state == Peer::State::Open) {
        // Reset reconnect backoff on successful connection
        reconnectCurrent_ = reconnectInitial_;

        if (metrics_) {
            auto& gauge = peer_state_gauge_family_ptr_->Add({{"source", source_}});
            gauge.Set(1.0);
        }
    } else if (state == Peer::State::Closed) {
        LOGWARNING(ert::tracing::Logger::warning(
            ert::tracing::Logger::asString("Diameter client: connection closed to %s:%d", host_.c_str(), port_),
            ERT_FILE_LOCATION));

        if (metrics_) {
            auto& gauge = peer_state_gauge_family_ptr_->Add({{"source", source_}});
            gauge.Set(0.0);
        }

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
        if (metrics_) {
            std::string commandCode = std::to_string(extractCommandCode(msg));
            std::string resultCode = std::to_string(extractResultCode(msg));
            auto& counter = answers_received_counter_family_ptr_->Add(
                {{"source", source_}, {"command_code", commandCode}, {"result_code", resultCode}});
            counter.Increment();
        }

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
    LOGINFORMATIONAL(ert::tracing::Logger::informational(
        ert::tracing::Logger::asString("Diameter client: reconnecting to %s:%d in %ld ms", host_.c_str(), port_,
                                       reconnectCurrent_.count()),
        ERT_FILE_LOCATION));

    reconnectTimer_.expires_after(reconnectCurrent_);
    reconnectTimer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;  // cancelled

        // Exponential backoff
        reconnectCurrent_ = std::min(reconnectCurrent_ * 2, reconnectMax_);

        // Reconnect
        connect(host_, port_);
    });
}

}  // namespace diametercomm
}  // namespace ert
