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
 * @file PeerConnection.hpp
 * @brief TCP/SCTP connection with Diameter message framing (RFC 6733 section 3).
 *
 * This class handles the low-level transport for Diameter messages:
 * - Async framing: reads version(1 byte) + length(3 bytes), then remaining body
 * - Supports both TCP and SCTP (one-to-one) transports
 * - Validates message length (minimum 20 bytes header)
 * - Provides async connect, read, and write operations via boost::asio
 *
 * @see Peer for the Diameter base protocol state machine built on top of this.
 */

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ert {
namespace diametercomm {

/**
 * Transport protocol for Diameter connections.
 * SCTP uses one-to-one style (RFC 6458) which has the same stream
 * semantics as TCP. The socket API is identical at the boost::asio level
 * because we assign the SCTP fd to a tcp::socket (both are SOCK_STREAM).
 */
enum class Transport { TCP, SCTP };

/**
 * TLS configuration for a Diameter connection (TCP transport only).
 *
 * Secures Diameter over TCP with TLS per RFC 6733. Diameter over SCTP would use
 * DTLS/SCTP (RFC 6083), which is NOT implemented here (see the h2diagent README
 * "gap" note): enabling TLS on an SCTP transport is a no-op by design.
 *
 * Server role: @c certFile + @c keyFile (+ optional @c keyPassword) enable TLS;
 * the server presents its certificate (server-auth). Client-certificate
 * verification (mTLS) happens only when @c verifyPeer is set together with a
 * @c caFile.
 *
 * Client role: @c enabled toggles TLS on the outbound connection. The server
 * certificate is verified only when @c verifyPeer is set together with a
 * @c caFile (otherwise the handshake still encrypts the channel but does not
 * authenticate the peer). @c certFile / @c keyFile are optional (client
 * certificate for mTLS).
 */
struct TlsConfig {
    bool enabled{false};      // master switch for this connection role
    std::string certFile;     // PEM certificate (chain)
    std::string keyFile;      // PEM private key
    std::string keyPassword;  // private key password (optional)
    std::string caFile;       // trusted CA to verify the peer (optional)
    bool verifyPeer{false};   // verify the peer certificate against caFile
};

/**
 * TCP/SCTP connection with Diameter message framing.
 *
 * Diameter messages are framed by reading the first 4 bytes (version + length)
 * to determine message size, then reading the remaining bytes.
 *
 * Internally uses boost::asio::ip::tcp::socket for both TCP and SCTP.
 * For SCTP, the socket fd is created with IPPROTO_SCTP (one-to-one style)
 * and assigned to the tcp::socket via native_handle. This works because
 * SCTP one-to-one has identical SOCK_STREAM semantics and boost::asio
 * uses epoll which is protocol-agnostic.
 */
class PeerConnection : public std::enable_shared_from_this<PeerConnection> {
   public:
    using Buffer = std::vector<uint8_t>;
    using MessageCallback = std::function<void(Buffer&&)>;
    using ErrorCallback = std::function<void(const boost::system::error_code&)>;

    /**
     * Construct from an existing connected socket (server-side: after accept).
     */
    explicit PeerConnection(boost::asio::ip::tcp::socket socket, Transport transport = Transport::TCP);

    /**
     * Construct with an io_context for client-side connections.
     */
    explicit PeerConnection(boost::asio::io_context& io, Transport transport = Transport::TCP);

    ~PeerConnection();

    // Non-copyable, movable
    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;
    PeerConnection(PeerConnection&&) = default;
    PeerConnection& operator=(PeerConnection&&) = default;

    /**
     * Connect to a remote Diameter peer (client-side).
     */
    void asyncConnect(const std::string& host, uint16_t port, std::function<void()> onConnected, ErrorCallback onError);

    /**
     * Start reading Diameter messages from the connection.
     */
    void startReading(MessageCallback onMessage, ErrorCallback onError);

    /**
     * Write a complete Diameter message.
     */
    void asyncWrite(Buffer msg, std::function<void()> onComplete = nullptr);

    /**
     * Gracefully close the connection.
     */
    void close();

    /**
     * Check if the connection is open.
     */
    bool isOpen() const;

    /**
     * Get the remote endpoint description (for logging).
     */
    std::string remoteEndpoint() const;

    /**
     * Get the underlying socket (for acceptor usage).
     */
    boost::asio::ip::tcp::socket& socket() { return socket_; }

    /**
     * Get the transport type.
     */
    Transport transport() const { return transport_; }

    /**
     * Wrap this connection in a TLS stream using the given SSL context.
     * @param ctx     Prebuilt SSL context (see makeServerContext/makeClientContext).
     * @param server  true for server-side handshake, false for client-side.
     * Only meaningful for TCP; callers must not enable it for SCTP.
     */
    void enableTls(std::shared_ptr<boost::asio::ssl::context> ctx, bool server);

    /**
     * Perform the server-side TLS handshake (after accept). Must be called
     * before startReading() on a TLS server connection. If TLS was not enabled
     * it simply invokes onDone (plain connection).
     */
    void asyncHandshakeServer(std::function<void()> onDone, ErrorCallback onError);

    /** Whether TLS has been enabled on this connection. */
    bool tlsEnabled() const { return tls_; }

    /** Build a server-side SSL context from the given TLS configuration. */
    static std::shared_ptr<boost::asio::ssl::context> makeServerContext(const TlsConfig& tls);

    /** Build a client-side SSL context from the given TLS configuration. */
    static std::shared_ptr<boost::asio::ssl::context> makeClientContext(const TlsConfig& tls);

   private:
    void doReadHeader();
    void doReadBody(uint32_t msgLen);

    boost::asio::ip::tcp::socket socket_;
    Transport transport_;
    std::array<uint8_t, 4> headerBuf_;
    Buffer readBuf_;

    MessageCallback onMessage_;
    ErrorCallback onError_;

    // --- TLS (TCP only) ---
    bool tls_{false};
    bool tlsServer_{false};
    std::shared_ptr<boost::asio::ssl::context> sslContext_;
    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> sslStream_;
};

}  // namespace diametercomm
}  // namespace ert
