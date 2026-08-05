/*
C++ DIAMETER COMMUNICATIONS LIBRARY
https://github.com/testillano/diametercomm
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#include <netinet/in.h>
#include <netinet/sctp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <ert/diametercomm/PeerConnection.hpp>
#include <sstream>

namespace ert {
namespace diametercomm {

// ============================================================================
// Construction / Destruction
// ============================================================================

PeerConnection::PeerConnection(boost::asio::ip::tcp::socket socket, Transport transport)
    : socket_(std::move(socket)), transport_(transport) {}

PeerConnection::PeerConnection(boost::asio::io_context& io, Transport transport) : socket_(io), transport_(transport) {}

PeerConnection::~PeerConnection() { close(); }

// ============================================================================
// asyncConnect (client-side)
// ============================================================================
void PeerConnection::asyncConnect(const std::string& host, uint16_t port, std::function<void()> onConnected,
                                  ErrorCallback onError) {
    auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(socket_.get_executor());

    auto self = shared_from_this();
    resolver->async_resolve(
        host, std::to_string(port),
        [self, resolver, onConnected, onError](const boost::system::error_code& ec,
                                               boost::asio::ip::tcp::resolver::results_type results) {
            if (ec) {
                if (onError) onError(ec);
                return;
            }

            if (self->transport_ == Transport::SCTP) {
                // For SCTP: create native socket and assign to tcp::socket
                auto it = results.begin();
                if (it == results.end()) {
                    if (onError) onError(boost::asio::error::host_not_found);
                    return;
                }
                int family = it->endpoint().address().is_v6() ? AF_INET6 : AF_INET;
                bool isV6 = (family == AF_INET6);
                int fd = ::socket(family, SOCK_STREAM, IPPROTO_SCTP);
                if (fd < 0) {
                    if (onError)
                        onError(boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported));
                    return;
                }
                boost::system::error_code assign_ec;
                auto proto = isV6 ? boost::asio::ip::tcp::v6() : boost::asio::ip::tcp::v4();
                self->socket_.assign(proto, fd, assign_ec);
                if (assign_ec) {
                    ::close(fd);
                    if (onError) onError(assign_ec);
                    return;
                }

                // Single-homing: connect the assigned SCTP socket to the single
                // resolved endpoint via the member async_connect. We must NOT use
                // the range free-function async_connect here: it closes/reopens
                // the socket for each candidate endpoint (reopening as TCP via
                // endpoint.protocol()), which would silently discard our SCTP fd.
                boost::asio::ip::tcp::endpoint endpoint = it->endpoint();
                self->socket_.async_connect(endpoint,
                                            [self, onConnected, onError](const boost::system::error_code& ec2) {
                                                if (ec2) {
                                                    if (onError) onError(ec2);
                                                    return;
                                                }
                                                if (onConnected) onConnected();
                                            });
                return;
            }

            // TCP: standard range connect (tries each resolved endpoint in turn).
            boost::asio::async_connect(self->socket_, results,
                                       [self, onConnected, onError](const boost::system::error_code& ec2,
                                                                    const boost::asio::ip::tcp::endpoint&) {
                                           if (ec2) {
                                               if (onError) onError(ec2);
                                               return;
                                           }
                                           // Disable Nagle for TCP
                                           self->socket_.set_option(boost::asio::ip::tcp::no_delay(true));
                                           if (onConnected) onConnected();
                                       });
        });
}

// ============================================================================
// startReading
// ============================================================================
void PeerConnection::startReading(MessageCallback onMessage, ErrorCallback onError) {
    onMessage_ = std::move(onMessage);
    onError_ = std::move(onError);
    doReadHeader();
}

// ============================================================================
// doReadHeader
// ============================================================================
void PeerConnection::doReadHeader() {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(headerBuf_),
                            [self](const boost::system::error_code& ec, std::size_t) {
                                if (ec) {
                                    if (self->onError_) self->onError_(ec);
                                    return;
                                }

                                uint32_t msgLen = (uint32_t(self->headerBuf_[1]) << 16) |
                                                  (uint32_t(self->headerBuf_[2]) << 8) | uint32_t(self->headerBuf_[3]);

                                if (msgLen < 20 || msgLen > 16777215) {
                                    auto err = boost::system::errc::make_error_code(boost::system::errc::message_size);
                                    if (self->onError_) self->onError_(err);
                                    return;
                                }

                                self->doReadBody(msgLen);
                            });
}

// ============================================================================
// doReadBody
// ============================================================================
void PeerConnection::doReadBody(uint32_t msgLen) {
    readBuf_.resize(msgLen);
    std::copy(headerBuf_.begin(), headerBuf_.end(), readBuf_.begin());

    uint32_t remaining = msgLen - 4;
    if (remaining == 0) {
        if (onMessage_) onMessage_(std::move(readBuf_));
        doReadHeader();
        return;
    }

    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(readBuf_.data() + 4, remaining),
                            [self](const boost::system::error_code& ec, std::size_t) {
                                if (ec) {
                                    if (self->onError_) self->onError_(ec);
                                    return;
                                }

                                if (self->onMessage_) {
                                    self->onMessage_(std::move(self->readBuf_));
                                }

                                self->doReadHeader();
                            });
}

// ============================================================================
// asyncWrite
// ============================================================================
void PeerConnection::asyncWrite(Buffer msg, std::function<void()> onComplete) {
    auto self = shared_from_this();
    auto msgPtr = std::make_shared<Buffer>(std::move(msg));

    boost::asio::async_write(socket_, boost::asio::buffer(*msgPtr),
                             [self, msgPtr, onComplete](const boost::system::error_code& ec, std::size_t) {
                                 if (ec) {
                                     if (self->onError_) self->onError_(ec);
                                     return;
                                 }
                                 if (onComplete) onComplete();
                             });
}

// ============================================================================
// close
// ============================================================================
void PeerConnection::close() {
    if (socket_.is_open()) {
        boost::system::error_code ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
}

// ============================================================================
// isOpen
// ============================================================================
bool PeerConnection::isOpen() const { return socket_.is_open(); }

// ============================================================================
// remoteEndpoint
// ============================================================================
std::string PeerConnection::remoteEndpoint() const {
    if (!socket_.is_open()) return "<closed>";
    try {
        auto ep = socket_.remote_endpoint();
        std::ostringstream oss;
        oss << ep.address().to_string() << ":" << ep.port();
        return oss.str();
    } catch (...) {
        return "<unknown>";
    }
}

}  // namespace diametercomm
}  // namespace ert
