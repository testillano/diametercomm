/*
C++ DIAMETER COMMUNICATIONS LIBRARY
https://github.com/testillano/diametercomm
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#include <netinet/in.h>
#include <netinet/sctp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <ert/diametercomm/DiameterServer.hpp>

namespace ert {
namespace diametercomm {

// Header helpers (local to this TU)
namespace {

uint32_t extractCommandCode(const Peer::Buffer& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[5]) << 16) | (uint32_t(msg[6]) << 8) | uint32_t(msg[7]);
}

uint32_t extractApplicationId(const Peer::Buffer& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[8]) << 24) | (uint32_t(msg[9]) << 16) | (uint32_t(msg[10]) << 8) | uint32_t(msg[11]);
}

uint32_t extractHopByHop(const Peer::Buffer& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[12]) << 24) | (uint32_t(msg[13]) << 16) | (uint32_t(msg[14]) << 8) | uint32_t(msg[15]);
}

void setHopByHop(Peer::Buffer& msg, uint32_t hbh) {
    msg[12] = static_cast<uint8_t>(hbh >> 24);
    msg[13] = static_cast<uint8_t>(hbh >> 16);
    msg[14] = static_cast<uint8_t>(hbh >> 8);
    msg[15] = static_cast<uint8_t>(hbh);
}

bool isRequest(const Peer::Buffer& msg) { return msg.size() >= 20 && (msg[4] & 0x80) != 0; }

uint32_t extractResultCode(const Peer::Buffer& msg) {
    // Result-Code AVP (code 268) is typically near the start of the answer.
    // AVP header: code(4) + flags(1) + length(3) [+ vendorId(4) if V bit set]
    // Walk AVPs to find code 268.
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

DiameterServer::DiameterServer(boost::asio::io_context& io, const Peer::Config& config, Transport transport)
    : io_(io), acceptor_(io), config_(config), transport_(transport) {
    // Build the TLS/TCP server context once (reused for every accepted peer).
    // TLS is not applied to SCTP (DTLS/SCTP is out of scope -- see README gap).
    if (config_.tls.enabled && transport_ == Transport::TCP) {
        serverCtx_ = PeerConnection::makeServerContext(config_.tls);
    }
}

DiameterServer::~DiameterServer() { close(); }

void DiameterServer::enableMetrics(ert::metrics::Metrics* metrics, const std::string& source) {
    metrics_ = metrics;

    if (metrics_) {
        source_ = source.empty() ? "diameter_server" : source;
        ert::metrics::labels_t familyLabels = {};

        requests_received_counter_family_ptr_ = &(metrics_->addCounterFamily(
            "diameter_server_requests_received_counter", "Diameter requests received counter", familyLabels));

        answers_sent_counter_family_ptr_ = &(metrics_->addCounterFamily("diameter_server_answers_sent_counter",
                                                                        "Diameter answers sent counter", familyLabels));

        peer_connections_counter_family_ptr_ =
            &(metrics_->addCounterFamily("diameter_server_peer_connections_counter",
                                         "Diameter server peer connection state transitions counter", familyLabels));

        active_peers_gauge_family_ptr_ = &(metrics_->addGaugeFamily(
            "diameter_server_active_peers_gauge", "Diameter server active peers gauge", familyLabels));

        // Bidirectional (RFC 6733): server-initiated requests + their answers.
        requests_sent_counter_family_ptr_ = &(metrics_->addCounterFamily(
            "diameter_server_requests_sent_counter", "Diameter server requests sent counter", familyLabels));

        answers_received_counter_family_ptr_ = &(metrics_->addCounterFamily(
            "diameter_server_answers_received_counter", "Diameter server answers received counter", familyLabels));
    }
}

void DiameterServer::listen(const std::string& bindAddress, uint16_t port) {
    if (transport_ == Transport::SCTP) {
        // For SCTP: create native socket, bind, listen, then assign to acceptor
        int family = AF_INET;
        int fd = ::socket(family, SOCK_STREAM, IPPROTO_SCTP);
        if (fd < 0) throw std::runtime_error("Failed to create SCTP socket");

        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr);

        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            throw std::runtime_error("Failed to bind SCTP socket");
        }
        if (::listen(fd, SOMAXCONN) < 0) {
            ::close(fd);
            throw std::runtime_error("Failed to listen on SCTP socket");
        }

        acceptor_.assign(boost::asio::ip::tcp::v4(), fd);
    } else {
        // TCP: standard boost::asio approach
        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address(bindAddress), port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
    }

    listening_ = true;
    doAccept();
}

void DiameterServer::stopListening() {
    listening_ = false;
    if (acceptor_.is_open()) {
        boost::system::error_code ec;
        acceptor_.close(ec);
    }
}

void DiameterServer::shutdown(uint32_t disconnectCause) {
    stopListening();

    // Copy peers under lock, then operate without lock to avoid deadlock:
    // peer->disconnect() may synchronously trigger setState(Closed) ->
    // onPeerStateChange() which needs peersMutex_.
    std::vector<std::shared_ptr<Peer>> peersCopy;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peersCopy = peers_;
    }
    for (auto& peer : peersCopy) {
        if (peer->state() == Peer::State::Open) {
            peer->disconnect(disconnectCause);
        }
    }
}

void DiameterServer::close() {
    stopListening();

    // Move peers out under lock, then close without lock to avoid deadlock:
    // peer->close() triggers setState(Closed) -> onPeerStateChange() which
    // needs peersMutex_.
    std::vector<std::shared_ptr<Peer>> peersCopy;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peersCopy = std::move(peers_);
        peers_.clear();
    }
    for (auto& peer : peersCopy) {
        peer->close();
    }
}

size_t DiameterServer::activePeerCount() const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    size_t count = 0;
    for (const auto& peer : peers_) {
        if (peer->state() == Peer::State::Open) ++count;
    }
    return count;
}

std::vector<std::shared_ptr<Peer>> DiameterServer::peers() const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    return peers_;
}

bool DiameterServer::sendAnswer(std::shared_ptr<Peer> peer, Peer::Buffer answer) {
    if (!peer) return false;

    if (metrics_) {
        std::string commandCode = std::to_string(extractCommandCode(answer));
        std::string resultCode = std::to_string(extractResultCode(answer));
        auto& counter = answers_sent_counter_family_ptr_->Add(
            {{"source", source_}, {"command_code", commandCode}, {"result_code", resultCode}});
        counter.Increment();
    }

    return peer->send(std::move(answer));
}

ert::metrics::labels_t DiameterServer::serverLabels(const std::string& commandCode, const std::string& applicationId,
                                                   const ert::metrics::labels_t& additionalLabels,
                                                   const std::string& resultCode) const {
    ert::metrics::labels_t labels = {
        {"source", source_}, {"command_code", commandCode}, {"application_id", applicationId}};
    if (!resultCode.empty()) labels["result_code"] = resultCode;
    for (const auto& kv : additionalLabels) labels[kv.first] = kv.second;
    return labels;
}

uint32_t DiameterServer::sendRequest(std::shared_ptr<Peer> peer, Peer::Buffer request, ResponseCallback onResponse,
                                     uint32_t timeoutMs, const ert::metrics::labels_t& additionalLabels) {
    if (!peer || request.size() < 20) return 0;

    std::string commandCode = std::to_string(extractCommandCode(request));
    std::string applicationId = std::to_string(extractApplicationId(request));

    // Assign hop-by-hop if the caller left it at 0.
    uint32_t hbh = extractHopByHop(request);
    if (hbh == 0) {
        hbh = peer->nextHopByHop();
        setHopByHop(request, hbh);
    }

    // Register the pending transaction (send time + labels reused for the answer).
    auto pending = std::make_shared<PendingRequest>(io_);
    pending->callback = std::move(onResponse);
    pending->sentAt = std::chrono::steady_clock::now();
    pending->additionalLabels = additionalLabels;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_[hbh] = pending;
    }

    // Arm the timeout.
    if (timeoutMs > 0) {
        pending->timer.expires_after(std::chrono::milliseconds(timeoutMs));
        pending->timer.async_wait([this, hbh](const boost::system::error_code& ec) {
            if (ec) return;  // cancelled (answer arrived)
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

    if (!peer->send(std::move(request))) {
        // Send failed: drop the pending transaction.
        std::lock_guard<std::mutex> lock(pendingMutex_);
        auto it = pending_.find(hbh);
        if (it != pending_.end()) {
            it->second->timer.cancel();
            pending_.erase(it);
        }
        return 0;
    }

    if (metrics_) {
        requests_sent_counter_family_ptr_->Add(serverLabels(commandCode, applicationId, additionalLabels)).Increment();
    }

    return hbh;
}

void DiameterServer::doAccept() {
    if (!listening_) return;

    auto acceptedSocket = std::make_shared<boost::asio::ip::tcp::socket>(io_);

    acceptor_.async_accept(*acceptedSocket, [this, acceptedSocket](const boost::system::error_code& ec) {
        if (ec) return;

        // Create PeerConnection from accepted socket
        auto connection = std::make_shared<PeerConnection>(std::move(*acceptedSocket), transport_);

        // Create + start the server-side Peer on a (possibly TLS-handshaked) connection.
        auto startPeer = [this](std::shared_ptr<PeerConnection> conn) {
            auto peer = std::make_shared<Peer>(conn, io_, config_);

            peer->setRequestCallback([this](std::shared_ptr<Peer> p, Peer::Buffer&& msg) {
                // Bidirectional (RFC 6733): an inbound ANSWER (R-bit clear) is
                // the correlated reply to a server-initiated request. Route it
                // to the pending map instead of treating it as a request.
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
                        if (metrics_) {
                            std::string commandCode = std::to_string(extractCommandCode(msg));
                            std::string applicationId = std::to_string(extractApplicationId(msg));
                            std::string resultCode = std::to_string(extractResultCode(msg));
                            answers_received_counter_family_ptr_
                                ->Add(serverLabels(commandCode, applicationId, req->additionalLabels, resultCode))
                                .Increment();
                        }
                        req->timer.cancel();
                        if (req->callback) req->callback(msg);
                        return;
                    }
                    // Uncorrelated answer: no pending transaction matches. Drop
                    // it (do not count as a received request nor deliver as one).
                    return;
                }

                if (metrics_) {
                    std::string commandCode = std::to_string(extractCommandCode(msg));
                    auto& counter = requests_received_counter_family_ptr_->Add(
                        {{"source", source_}, {"command_code", commandCode}});
                    counter.Increment();
                }
                if (onRequest_) onRequest_(std::move(p), std::move(msg));
            });

            peer->setStateCallback(
                [this](std::shared_ptr<Peer> p, Peer::State state) { onPeerStateChange(std::move(p), state); });

            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                peers_.push_back(peer);
            }

            peer->start();
        };

        if (serverCtx_) {
            // TLS/TCP: handshake first; only create/start the Peer on success.
            connection->enableTls(serverCtx_, /*server*/ true);
            connection->asyncHandshakeServer([startPeer, connection]() { startPeer(connection); },
                                             [](const boost::system::error_code&) { /* drop on handshake failure */ });
        } else {
            startPeer(connection);
        }

        doAccept();
    });
}

void DiameterServer::onPeerStateChange(std::shared_ptr<Peer> peer, Peer::State state) {
    if (metrics_) {
        if (state == Peer::State::Open) {
            auto& counter = peer_connections_counter_family_ptr_->Add({{"source", source_}, {"state", "open"}});
            counter.Increment();
        } else if (state == Peer::State::Closed) {
            auto& counter = peer_connections_counter_family_ptr_->Add({{"source", source_}, {"state", "closed"}});
            counter.Increment();
        }
    }

    if (onPeerEvent_) {
        onPeerEvent_(peer, state);
    }

    if (state == Peer::State::Closed) {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peers_.erase(std::remove(peers_.begin(), peers_.end(), peer), peers_.end());
    }

    // Update active peers gauge after state change (activePeerCount() locks internally)
    if (metrics_) {
        size_t count = activePeerCount();
        auto& gauge = active_peers_gauge_family_ptr_->Add({{"source", source_}});
        gauge.Set(static_cast<double>(count));
    }
}

}  // namespace diametercomm
}  // namespace ert
