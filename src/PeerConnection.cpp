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
#include <ert/tracing/Logger.hpp>
#include <sstream>

namespace ert {
namespace diametercomm {

// ============================================================================
// Construction / Destruction
// ============================================================================

// Every socket operation and handler of a connection runs on its strand: the
// io_context may run on many threads and writes are requested from foreign
// threads (e.g. the gateway HTTP/2 thread). Without it, overlapping
// async_write interleave bytes on the wire (TCP) or corrupt the TLS engine.
PeerConnection::PeerConnection(boost::asio::ip::tcp::socket socket, Transport transport)
    : socket_(std::move(socket)), strand_(boost::asio::make_strand(socket_.get_executor())), transport_(transport) {}

PeerConnection::PeerConnection(boost::asio::io_context& io, Transport transport)
    : socket_(io), strand_(boost::asio::make_strand(socket_.get_executor())), transport_(transport) {}

PeerConnection::~PeerConnection() { close(); }

// ============================================================================
// TLS (TCP only): context factories, enable and server-side handshake
// ============================================================================
std::shared_ptr<boost::asio::ssl::context> PeerConnection::makeServerContext(const TlsConfig& tls) {
    auto ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
    ctx->set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 |
                     boost::asio::ssl::context::no_sslv3 | boost::asio::ssl::context::single_dh_use);
    if (!tls.keyPassword.empty()) {
        ctx->set_password_callback(
            [pw = tls.keyPassword](std::size_t, boost::asio::ssl::context::password_purpose) { return pw; });
    }
    ctx->use_certificate_chain_file(tls.certFile);
    ctx->use_private_key_file(tls.keyFile, boost::asio::ssl::context::pem);
    if (tls.verifyPeer && !tls.caFile.empty()) {
        ctx->load_verify_file(tls.caFile);
        ctx->set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert);
    } else {
        ctx->set_verify_mode(boost::asio::ssl::verify_none);
    }
    return ctx;
}

std::shared_ptr<boost::asio::ssl::context> PeerConnection::makeClientContext(const TlsConfig& tls) {
    auto ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
    ctx->set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 |
                     boost::asio::ssl::context::no_sslv3);
    if (tls.verifyPeer && !tls.caFile.empty()) {
        ctx->load_verify_file(tls.caFile);
        ctx->set_verify_mode(boost::asio::ssl::verify_peer);
    } else {
        ctx->set_verify_mode(boost::asio::ssl::verify_none);
    }
    // Optional client certificate (mTLS)
    if (!tls.certFile.empty() && !tls.keyFile.empty()) {
        if (!tls.keyPassword.empty()) {
            ctx->set_password_callback(
                [pw = tls.keyPassword](std::size_t, boost::asio::ssl::context::password_purpose) { return pw; });
        }
        ctx->use_certificate_chain_file(tls.certFile);
        ctx->use_private_key_file(tls.keyFile, boost::asio::ssl::context::pem);
    }
    return ctx;
}

void PeerConnection::enableTls(std::shared_ptr<boost::asio::ssl::context> ctx, bool server) {
    if (!ctx) return;
    sslContext_ = std::move(ctx);
    tlsServer_ = server;
    tls_ = true;
    sslStream_ = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>>(socket_, *sslContext_);
}

void PeerConnection::asyncHandshakeServer(std::function<void()> onDone, ErrorCallback onError) {
    if (!tls_) {
        if (onDone) onDone();
        return;
    }
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, onDone, onError]() {
        self->sslStream_->async_handshake(
            boost::asio::ssl::stream_base::server,
            boost::asio::bind_executor(self->strand_, [self, onDone, onError](const boost::system::error_code& ec) {
                if (ec) {
                    if (onError) onError(ec);
                    return;
                }
                if (onDone) onDone();
            }));
    });
}

// ============================================================================
// deriveSourceAddress (client-side helper)
// ============================================================================
//
// Returns the local source IP the kernel would use to reach `peer` -- i.e. the
// exact `src` field of `ip route get <peer>` -- or "" if it cannot be resolved.
//
// WHY THIS EXISTS (SCTP multihoming vs a multi-interface host):
//   A client socket left unbound (INADDR_ANY / 0.0.0.0) does NOT send from
//   "0.0.0.0" -- that is a wildcard meaning "let the kernel choose". The kernel
//   picks the egress interface from the ROUTING TABLE toward the destination and
//   uses that interface's address as the packet source. For TCP that is the end
//   of the story (one source address, replies come back to it).
//
//   SCTP is different: it is multihomed. In its INIT chunk the client ADVERTISES
//   its local addresses as candidate paths. An unbound SCTP socket on a host with
//   several interfaces advertises ALL of them -- including addresses that are NOT
//   routable from the peer (e.g. Docker's docker0 192.168.x, bridges 172.17.x,
//   or management NICs). The peer then cannot return INIT-ACK / verify those
//   paths and the association stalls in COOKIE_WAIT (handshake never completes).
//
//   To avoid that we SINGLE-HOME the association: bind the socket to ONE local
//   address before connecting, so the INIT advertises only that address. The
//   right address to advertise is precisely the egress source toward the peer --
//   advertising anything else (an address the traffic does not actually leave
//   from) would itself break SCTP path verification.
//
// HOW IT WORKS (connected-UDP + getsockname, no traffic sent):
//   Creating a UDP socket and `connect()`-ing it to the peer does NOT send any
//   datagram; it only makes the kernel resolve the route and bind the socket's
//   local end to the source address it would use. `getsockname()` (via
//   local_endpoint()) then reveals that source IP. This mirrors `ip route get`
//   without shelling out to a binary, and works for both IPv4 and IPv6.
//
// NOTE: this derives the address of the route the kernel ALREADY has. It cannot
//   fix a wrong/missing route or a peer that only accepts traffic via a different
//   path -- those are network/routing problems to solve outside the client (or by
//   pinning an explicit source via Peer::Config::localAddress; see asyncConnect).
std::string PeerConnection::deriveSourceAddress(const boost::asio::ip::address& peer) {
    try {
        boost::asio::io_context tmp;
        boost::asio::ip::udp::socket probe(tmp, peer.is_v6() ? boost::asio::ip::udp::v6() : boost::asio::ip::udp::v4());
        boost::system::error_code ec;
        probe.connect(boost::asio::ip::udp::endpoint(peer, 9 /* discard; no datagram sent */), ec);
        if (ec) return "";
        auto local = probe.local_endpoint(ec);
        if (ec) return "";
        return local.address().to_string();
    } catch (...) {
        return "";
    }
}

// ============================================================================
// asyncConnect (client-side)
// ============================================================================
void PeerConnection::asyncConnect(const std::string& host, uint16_t port, std::function<void()> onConnected,
                                  ErrorCallback onError, const std::string& localAddress,
                                  const SctpOptions& sctpOptions) {
    auto self = shared_from_this();

    // A list of peer addresses (multihoming): SCTP only, no resolution (addresses).
    if (sctp::isAddressList(host)) {
        std::vector<boost::asio::ip::address> peers;
        const std::string err = sctp::parseAddressList(host, peers);
        if (transport_ != Transport::SCTP || !err.empty()) {
            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("Invalid peer address list '%s': %s", host.c_str(),
                                               transport_ != Transport::SCTP ? "a list needs SCTP" : err.c_str()),
                ERT_FILE_LOCATION));
            boost::asio::post(strand_, [onError]() {
                if (onError) onError(boost::asio::error::invalid_argument);
            });
            return;
        }
        boost::asio::post(strand_, [self, peers, port, onConnected, onError, localAddress, sctpOptions]() {
            self->connectSctp(peers, port, onConnected, onError, localAddress, sctpOptions);
        });
        return;
    }

    auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(socket_.get_executor());
    resolver->async_resolve(
        host, std::to_string(port),
        boost::asio::bind_executor(strand_, [self, resolver, onConnected, onError, localAddress, port, sctpOptions](
                                                const boost::system::error_code& ec,
                                                boost::asio::ip::tcp::resolver::results_type results) {
            if (ec) {
                if (onError) onError(ec);
                return;
            }

            if (self->transport_ == Transport::SCTP) {
                auto it = results.begin();
                if (it == results.end()) {
                    if (onError) onError(boost::asio::error::host_not_found);
                    return;
                }
                self->connectSctp({it->endpoint().address()}, port, onConnected, onError, localAddress, sctpOptions);
                return;
            }

            // TCP: standard range connect (tries each resolved endpoint in turn).
            boost::asio::async_connect(
                self->socket_, results,
                boost::asio::bind_executor(
                    self->strand_, [self, onConnected, onError](const boost::system::error_code& ec2,
                                                                const boost::asio::ip::tcp::endpoint&) {
                        if (ec2) {
                            if (onError) onError(ec2);
                            return;
                        }
                        // Disable Nagle for TCP
                        self->socket_.set_option(boost::asio::ip::tcp::no_delay(true));
                        if (self->tls_) {
                            // TLS/TCP: perform client handshake before signalling connected.
                            auto onHandshake = [self, onConnected, onError](const boost::system::error_code& hec) {
                                if (hec) {
                                    if (onError) onError(hec);
                                    return;
                                }
                                if (onConnected) onConnected();
                            };
                            self->sslStream_->async_handshake(
                                boost::asio::ssl::stream_base::client,
                                boost::asio::bind_executor(self->strand_, std::move(onHandshake)));
                        } else {
                            if (onConnected) onConnected();
                        }
                    }));
        }));
}

// ============================================================================
// connectSctp (strand): one-to-one SCTP socket, local binding (single or
// multihomed), options, connect to one or several peer addresses
// ============================================================================
void PeerConnection::connectSctp(std::vector<boost::asio::ip::address> peers, uint16_t port,
                                 std::function<void()> onConnected, ErrorCallback onError,
                                 const std::string& localAddress, const SctpOptions& sctpOptions) {
    auto self = shared_from_this();
    const bool isV6 = peers.front().is_v6();
    int fd = ::socket(isV6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (fd < 0) {
        if (onError) onError(boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported));
        return;
    }
    boost::system::error_code assign_ec;
    socket_.assign(isV6 ? boost::asio::ip::tcp::v6() : boost::asio::ip::tcp::v4(), fd, assign_ec);
    if (assign_ec) {
        ::close(fd);
        if (onError) onError(assign_ec);
        return;
    }

    const std::string optionsErr = sctp::applyOptions(fd, sctpOptions);
    if (!optionsErr.empty()) {
        LOGWARNING(ert::tracing::Logger::warning(
            ert::tracing::Logger::asString("SCTP options not applied: %s", optionsErr.c_str()), ERT_FILE_LOCATION));
    }

    // Local addresses. Single-homing (one address: explicit, or derived from the
    // route toward the first peer) avoids advertising non-routable local
    // interfaces (docker0, bridges) in the INIT, which would stall the handshake
    // in COOKIE_WAIT (see deriveSourceAddress). Multihoming (a list): every given
    // address is bound and advertised (SCTP_SOCKOPT_BINDX_ADD).
    {
        std::vector<boost::asio::ip::address> locals;
        std::string source = localAddress;
        const bool explicitPin = !source.empty();
        if (source.empty()) source = PeerConnection::deriveSourceAddress(peers.front());
        if (!source.empty()) {
            const std::string err = sctp::parseAddressList(source, locals);
            if (!err.empty()) {
                if (explicitPin) {
                    LOGWARNING(ert::tracing::Logger::warning(
                        ert::tracing::Logger::asString("SCTP client local address '%s': %s; leaving socket unbound",
                                                       source.c_str(), err.c_str()),
                        ERT_FILE_LOCATION));
                }
                locals.clear();
            } else {
                const std::string bindErr = sctp::bindAll(fd, locals, 0);
                if (!bindErr.empty()) {
                    LOGWARNING(ert::tracing::Logger::warning(
                        ert::tracing::Logger::asString("SCTP client bind failed: %s", bindErr.c_str()),
                        ERT_FILE_LOCATION));
                    if (onError)
                        onError(boost::system::errc::make_error_code(boost::system::errc::address_not_available));
                    return;
                }
                LOGWARNING(ert::tracing::Logger::warning(
                    ert::tracing::Logger::asString("SCTP client source address(es) (advertised in INIT): %s [%s]",
                                                   source.c_str(), explicitPin ? "explicit" : "auto-derived"),
                    ERT_FILE_LOCATION));
            }
        } else {
            LOGWARNING(
                ert::tracing::Logger::warning("SCTP client source address could not be derived; socket left unbound "
                                              "(INIT will advertise all local addresses)",
                                              ERT_FILE_LOCATION));
        }
    }

    // Once connected: the primary peer address (when asked), then the caller.
    auto connected = [self, port, onConnected, sctpOptions]() {
        if (!sctpOptions.primaryPeerAddress.empty()) {
            boost::system::error_code ec;
            const auto primary = boost::asio::ip::make_address(sctpOptions.primaryPeerAddress, ec);
            const std::string err =
                ec ? "invalid address" : sctp::setPrimary(self->socket_.native_handle(), primary, port);
            if (!err.empty()) {
                LOGWARNING(ert::tracing::Logger::warning(
                    ert::tracing::Logger::asString("SCTP primary peer address '%s' not set: %s",
                                                   sctpOptions.primaryPeerAddress.c_str(), err.c_str()),
                    ERT_FILE_LOCATION));
            }
        }
        if (onConnected) onConnected();
    };

    if (peers.size() == 1) {
        // Member async_connect (the range free function would close/reopen the
        // socket as TCP for each candidate, discarding our SCTP fd).
        boost::asio::ip::tcp::endpoint endpoint(peers.front(), port);
        socket_.async_connect(
            endpoint, boost::asio::bind_executor(strand_, [onError, connected](const boost::system::error_code& ec) {
                if (ec) {
                    if (onError) onError(ec);
                    return;
                }
                connected();
            }));
        return;
    }

    // Several peer addresses: non blocking SCTP_SOCKOPT_CONNECTX, then wait for
    // the socket to be writable and read the connection outcome (SO_ERROR).
    boost::system::error_code nb_ec;
    socket_.non_blocking(true, nb_ec);
    const int err = sctp::connectAll(fd, peers, port);
    if (err != 0) {
        if (onError) onError(boost::system::error_code(err, boost::system::system_category()));
        return;
    }
    socket_.async_wait(
        boost::asio::ip::tcp::socket::wait_write,
        boost::asio::bind_executor(strand_, [self, onError, connected](const boost::system::error_code& ec) {
            if (ec) {
                if (onError) onError(ec);
                return;
            }
            int soError = 0;
            socklen_t len = sizeof(soError);
            ::getsockopt(self->socket_.native_handle(), SOL_SOCKET, SO_ERROR, &soError, &len);
            if (soError != 0) {
                if (onError) onError(boost::system::error_code(soError, boost::system::system_category()));
                return;
            }
            connected();
        }));
}

std::vector<sctp::PathInfo> PeerConnection::sctpPaths() const {
    if (transport_ != Transport::SCTP || !socket_.is_open()) return {};
    return sctp::paths(const_cast<boost::asio::ip::tcp::socket&>(socket_).native_handle());
}

std::vector<std::string> PeerConnection::sctpLocalAddresses() const {
    if (transport_ != Transport::SCTP || !socket_.is_open()) return {};
    return sctp::localAddresses(const_cast<boost::asio::ip::tcp::socket&>(socket_).native_handle());
}

// ============================================================================
// startReading
// ============================================================================
void PeerConnection::startReading(MessageCallback onMessage, ErrorCallback onError) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, onMessage = std::move(onMessage), onError = std::move(onError)]() mutable {
        self->onMessage_ = std::move(onMessage);
        self->onError_ = std::move(onError);
        self->doReadHeader();
    });
}

// ============================================================================
// reportError (strand only): first error wins, pending writes are dropped
// ============================================================================
void PeerConnection::reportError(const boost::system::error_code& ec) {
    if (errorReported_) return;
    errorReported_ = true;
    writeQueue_.clear();
    if (onError_) onError_(ec);
}

// ============================================================================
// doReadHeader
// ============================================================================
void PeerConnection::doReadHeader() {
    auto self = shared_from_this();
    auto handler = boost::asio::bind_executor(strand_, [self](const boost::system::error_code& ec, std::size_t) {
        if (ec) {
            self->reportError(ec);
            return;
        }

        uint32_t msgLen = (uint32_t(self->headerBuf_[1]) << 16) | (uint32_t(self->headerBuf_[2]) << 8) |
                          uint32_t(self->headerBuf_[3]);

        if (msgLen < 20 || msgLen > 16777215) {
            self->reportError(boost::system::errc::make_error_code(boost::system::errc::message_size));
            return;
        }

        self->doReadBody(msgLen);
    });
    if (tls_) {
        boost::asio::async_read(*sslStream_, boost::asio::buffer(headerBuf_), std::move(handler));
    } else {
        boost::asio::async_read(socket_, boost::asio::buffer(headerBuf_), std::move(handler));
    }
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
    auto handler = boost::asio::bind_executor(strand_, [self](const boost::system::error_code& ec, std::size_t) {
        if (ec) {
            self->reportError(ec);
            return;
        }

        if (self->onMessage_) {
            self->onMessage_(std::move(self->readBuf_));
        }

        self->doReadHeader();
    });
    if (tls_) {
        boost::asio::async_read(*sslStream_, boost::asio::buffer(readBuf_.data() + 4, remaining), std::move(handler));
    } else {
        boost::asio::async_read(socket_, boost::asio::buffer(readBuf_.data() + 4, remaining), std::move(handler));
    }
}

// ============================================================================
// asyncWrite
// ============================================================================
void PeerConnection::asyncWrite(Buffer msg, std::function<void()> onComplete) {
    if (closed_) return;
    auto self = shared_from_this();
    PendingWrite pending{std::make_shared<Buffer>(std::move(msg)), std::move(onComplete)};
    boost::asio::post(strand_, [self, pending = std::move(pending)]() mutable {
        if (self->closed_ || self->errorReported_) return;
        self->writeQueue_.push_back(std::move(pending));
        if (!self->writing_) self->doWrite();
    });
}

// ============================================================================
// doWrite (strand only): write the queue head; the next one starts when it
// completes, so there is never more than one async_write on the socket.
// ============================================================================
void PeerConnection::doWrite() {
    writing_ = true;
    auto self = shared_from_this();
    auto msgPtr = writeQueue_.front().msg;  // keeps the buffer alive until completion

    auto onWritten = [self, msgPtr](const boost::system::error_code& ec, std::size_t) {
        self->writing_ = false;
        if (ec) {
            self->reportError(ec);
            return;
        }
        if (self->writeQueue_.empty()) return;  // dropped meanwhile (error reported on read side)
        auto onComplete = std::move(self->writeQueue_.front().onComplete);
        self->writeQueue_.pop_front();
        if (!self->writeQueue_.empty()) self->doWrite();
        if (onComplete) onComplete();
    };
    auto handler = boost::asio::bind_executor(strand_, std::move(onWritten));
    if (tls_) {
        boost::asio::async_write(*sslStream_, boost::asio::buffer(*msgPtr), std::move(handler));
    } else {
        boost::asio::async_write(socket_, boost::asio::buffer(*msgPtr), std::move(handler));
    }
}

// ============================================================================
// close
// ============================================================================
void PeerConnection::close() {
    closed_ = true;
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

std::string PeerConnection::localEndpoint() const {
    if (!socket_.is_open()) return "<closed>";
    boost::system::error_code ec;
    auto ep = socket_.local_endpoint(ec);
    if (ec) return "<unknown>";
    std::ostringstream oss;
    oss << ep.address().to_string() << ":" << ep.port();
    return oss.str();
}

}  // namespace diametercomm
}  // namespace ert
