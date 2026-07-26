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

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <boost/asio.hpp>

namespace ert
{
namespace diametercomm
{

/**
 * Transport protocol for Diameter connections.
 * SCTP uses one-to-one style (RFC 6458) which has the same stream
 * semantics as TCP. The socket API is identical at the boost::asio level
 * because we assign the SCTP fd to a tcp::socket (both are SOCK_STREAM).
 */
enum class Transport { TCP, SCTP };

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
    explicit PeerConnection(boost::asio::ip::tcp::socket socket,
                            Transport transport = Transport::TCP);

    /**
     * Construct with an io_context for client-side connections.
     */
    explicit PeerConnection(boost::asio::io_context& io,
                            Transport transport = Transport::TCP);

    ~PeerConnection();

    // Non-copyable, movable
    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;
    PeerConnection(PeerConnection&&) = default;
    PeerConnection& operator=(PeerConnection&&) = default;

    /**
     * Connect to a remote Diameter peer (client-side).
     */
    void asyncConnect(const std::string& host, uint16_t port,
                      std::function<void()> onConnected,
                      ErrorCallback onError);

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

private:

    void doReadHeader();
    void doReadBody(uint32_t msgLen);

    boost::asio::ip::tcp::socket socket_;
    Transport transport_;
    std::array<uint8_t, 4> headerBuf_;
    Buffer readBuf_;

    MessageCallback onMessage_;
    ErrorCallback onError_;
};

} // namespace diametercomm
} // namespace ert
