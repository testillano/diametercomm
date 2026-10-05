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

PeerConnection::PeerConnection(boost::asio::ip::tcp::socket socket, Transport transport)
    : socket_(std::move(socket)), transport_(transport) {}

PeerConnection::PeerConnection(boost::asio::io_context& io, Transport transport) : socket_(io), transport_(transport) {}

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
    sslStream_->async_handshake(boost::asio::ssl::stream_base::server,
                                [self, onDone, onError](const boost::system::error_code& ec) {
                                    if (ec) {
                                        if (onError) onError(ec);
                                        return;
                                    }
                                    if (onDone) onDone();
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
                                  ErrorCallback onError, const std::string& localAddress) {
    auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(socket_.get_executor());

    auto self = shared_from_this();
    resolver->async_resolve(
        host, std::to_string(port),
        [self, resolver, onConnected, onError, localAddress](const boost::system::error_code& ec,
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

                // SCTP single-homing: bind the socket to ONE local source
                // address before connecting, so the INIT advertises only that
                // address (not every local interface). See deriveSourceAddress()
                // above for the full rationale (SCTP multihoming would otherwise
                // advertise non-routable addresses like docker0 and stall the
                // handshake in COOKIE_WAIT).
                //
                // Address selection:
                //   - localAddress NON-EMPTY  -> explicit override: bind exactly
                //     to it. Use this to PIN the egress interface when the kernel
                //     route would pick a path the peer does not accept (asymmetric
                //     routing, or a VIP reachable only via a specific interface).
                //     The caller is responsible for providing an address the peer
                //     can actually reach (and that the host can egress from).
                //   - localAddress EMPTY (default) -> auto-derive the egress source
                //     toward the peer (deriveSourceAddress). This is the kernel's
                //     own route decision, so the advertised address matches where
                //     the traffic actually leaves from.
                //   - derivation fails -> leave the socket unbound (legacy
                //     behavior); better to try than to abort the connect.
                //
                // A failed bind (e.g. the given address is not local) is surfaced
                // via onError rather than silently ignored.
                {
                    std::string local = localAddress;
                    bool explicitPin = !local.empty();
                    if (local.empty()) local = PeerConnection::deriveSourceAddress(it->endpoint().address());
                    if (!local.empty()) {
                        boost::system::error_code addr_ec;
                        auto laddr = boost::asio::ip::make_address(local, addr_ec);
                        if (!addr_ec) {
                            boost::system::error_code bind_ec;
                            self->socket_.bind(boost::asio::ip::tcp::endpoint(laddr, 0), bind_ec);
                            if (bind_ec) {
                                if (onError) onError(bind_ec);
                                return;
                            }
                            // Startup visibility: report the single local source address the
                            // SCTP INIT will advertise, and whether it was pinned explicitly
                            // (--diameter-client-local-host / Config::localAddress) or
                            // auto-derived from the route toward the peer.
                            LOGWARNING(ert::tracing::Logger::warning(
                                ert::tracing::Logger::asString(
                                    "SCTP client source address (advertised in INIT): %s [%s]", local.c_str(),
                                    explicitPin ? "explicit" : "auto-derived"),
                                ERT_FILE_LOCATION));
                        } else if (explicitPin) {
                            // An explicit address that does not parse is a config error worth flagging.
                            LOGWARNING(ert::tracing::Logger::warning(
                                ert::tracing::Logger::asString(
                                    "SCTP client local address '%s' is not a valid IP; leaving socket unbound",
                                    local.c_str()),
                                ERT_FILE_LOCATION));
                        }
                    } else {
                        // Could not determine a source address: unbound (legacy multihomed INIT).
                        LOGWARNING(ert::tracing::Logger::warning(
                            "SCTP client source address could not be derived; socket left unbound "
                            "(INIT will advertise all local addresses)",
                            ERT_FILE_LOCATION));
                    }
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
                                           if (self->tls_) {
                                               // TLS/TCP: perform client handshake before signalling connected.
                                               self->sslStream_->async_handshake(
                                                   boost::asio::ssl::stream_base::client,
                                                   [self, onConnected, onError](const boost::system::error_code& hec) {
                                                       if (hec) {
                                                           if (onError) onError(hec);
                                                           return;
                                                       }
                                                       if (onConnected) onConnected();
                                                   });
                                           } else {
                                               if (onConnected) onConnected();
                                           }
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
    auto handler = [self](const boost::system::error_code& ec, std::size_t) {
        if (ec) {
            if (self->onError_) self->onError_(ec);
            return;
        }

        uint32_t msgLen = (uint32_t(self->headerBuf_[1]) << 16) | (uint32_t(self->headerBuf_[2]) << 8) |
                          uint32_t(self->headerBuf_[3]);

        if (msgLen < 20 || msgLen > 16777215) {
            auto err = boost::system::errc::make_error_code(boost::system::errc::message_size);
            if (self->onError_) self->onError_(err);
            return;
        }

        self->doReadBody(msgLen);
    };
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
    auto handler = [self](const boost::system::error_code& ec, std::size_t) {
        if (ec) {
            if (self->onError_) self->onError_(ec);
            return;
        }

        if (self->onMessage_) {
            self->onMessage_(std::move(self->readBuf_));
        }

        self->doReadHeader();
    };
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
    auto self = shared_from_this();
    auto msgPtr = std::make_shared<Buffer>(std::move(msg));

    auto handler = [self, msgPtr, onComplete](const boost::system::error_code& ec, std::size_t) {
        if (ec) {
            if (self->onError_) self->onError_(ec);
            return;
        }
        if (onComplete) onComplete();
    };
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
