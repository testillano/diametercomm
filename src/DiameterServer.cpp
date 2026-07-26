/*
C++ DIAMETER COMMUNICATIONS LIBRARY
https://github.com/testillano/diametercomm
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#include <ert/diametercomm/DiameterServer.hpp>

#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <unistd.h>

namespace ert
{
namespace diametercomm
{

DiameterServer::DiameterServer(boost::asio::io_context& io, const Peer::Config& config,
                               Transport transport)
    : io_(io)
    , acceptor_(io)
    , config_(config)
    , transport_(transport)
{
}

DiameterServer::~DiameterServer() {
    close();
}

void DiameterServer::listen(const std::string& bindAddress, uint16_t port) {
    if (transport_ == Transport::SCTP) {
        // For SCTP: create native socket, bind, listen, then assign to acceptor
        int family = AF_INET;
        int fd = ::socket(family, SOCK_STREAM, IPPROTO_SCTP);
        if (fd < 0) throw std::runtime_error("Failed to create SCTP socket");

        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr{};
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
        boost::asio::ip::tcp::endpoint endpoint(
            boost::asio::ip::address::from_string(bindAddress), port);
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

void DiameterServer::doAccept() {
    if (!listening_) return;

    auto acceptedSocket = std::make_shared<boost::asio::ip::tcp::socket>(io_);

    acceptor_.async_accept(*acceptedSocket,
        [this, acceptedSocket](const boost::system::error_code& ec) {
            if (ec) return;

            // Create PeerConnection from accepted socket
            auto connection = std::make_shared<PeerConnection>(
                std::move(*acceptedSocket), transport_);

            // Create a new Peer in server mode
            auto peer = std::make_shared<Peer>(connection, io_, config_);

            peer->setRequestCallback(
                [this](std::shared_ptr<Peer> p, Peer::Buffer&& msg) {
                    if (onRequest_) onRequest_(std::move(p), std::move(msg));
                });

            peer->setStateCallback(
                [this](std::shared_ptr<Peer> p, Peer::State state) {
                    onPeerStateChange(std::move(p), state);
                });

            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                peers_.push_back(peer);
            }

            peer->start();
            doAccept();
        });
}

void DiameterServer::onPeerStateChange(std::shared_ptr<Peer> peer, Peer::State state) {
    if (onPeerEvent_) {
        onPeerEvent_(peer, state);
    }

    if (state == Peer::State::Closed) {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peers_.erase(
            std::remove(peers_.begin(), peers_.end(), peer),
            peers_.end());
    }
}

} // namespace diametercomm
} // namespace ert
