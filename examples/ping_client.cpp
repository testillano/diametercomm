/*
 ___________________________________________________________________________
|                                                                           |
|   diametercomm example: ping_client                                       |
|                                                                           |
|   Connects to a Diameter peer, sends a single request (custom command     |
|   code), waits for the answer, prints it and exits.                       |
|                                                                           |
|   The request is hand-crafted (no diametercodec dependency) with a        |
|   minimal valid Diameter header + Origin-Host AVP.                        |
|                                                                           |
|   Usage:                                                                  |
|     ping_client [host] [port] [command_code]                              |
|     ping_client                        # 127.0.0.1:3868, CC=999          |
|     ping_client 10.0.0.1 3868 272                                        |
|___________________________________________________________________________|
*/

#include <boost/asio.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ert/diametercomm/DiameterClient.hpp>
#include <ert/diametercomm/Peer.hpp>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

// Diameter header layout (RFC 6733 section 3):
//   Version (1) + Message Length (3) + Flags (1) + Command-Code (3) +
//   Application-Id (4) + Hop-by-Hop (4) + End-to-End (4) = 20 bytes
constexpr size_t HEADER_SIZE = 20;
constexpr uint8_t FLAG_REQUEST = 0x80;

using Buffer = ert::diametercomm::Peer::Buffer;

/**
 * Build a minimal Diameter request with just an Origin-Host AVP.
 * This is "de estar por casa" -- hand-crafted bytes, no codec.
 */
Buffer buildRequest(uint32_t commandCode, uint32_t appId, const std::string& originHost) {
    // AVP: Origin-Host (264), mandatory, OctetString
    // AVP header: Code(4) + Flags(1) + Length(3) = 8 bytes
    uint32_t avpDataLen = static_cast<uint32_t>(originHost.size());
    uint32_t avpTotalLen = 8 + avpDataLen;
    // Pad to 4-byte boundary
    uint32_t avpPadded = (avpTotalLen + 3) & ~3u;

    uint32_t msgLen = HEADER_SIZE + avpPadded;
    Buffer msg(msgLen, 0);

    // -- Header --
    // Version = 1
    msg[0] = 0x01;
    // Message Length (3 bytes)
    msg[1] = (msgLen >> 16) & 0xFF;
    msg[2] = (msgLen >> 8) & 0xFF;
    msg[3] = msgLen & 0xFF;
    // Flags: Request
    msg[4] = FLAG_REQUEST;
    // Command-Code (3 bytes)
    msg[5] = (commandCode >> 16) & 0xFF;
    msg[6] = (commandCode >> 8) & 0xFF;
    msg[7] = commandCode & 0xFF;
    // Application-Id (4 bytes)
    msg[8] = (appId >> 24) & 0xFF;
    msg[9] = (appId >> 16) & 0xFF;
    msg[10] = (appId >> 8) & 0xFF;
    msg[11] = appId & 0xFF;
    // Hop-by-Hop and End-to-End left as 0 (library fills them)

    // -- AVP: Origin-Host (264) --
    size_t offset = HEADER_SIZE;
    uint32_t avpCode = 264;
    msg[offset + 0] = (avpCode >> 24) & 0xFF;
    msg[offset + 1] = (avpCode >> 16) & 0xFF;
    msg[offset + 2] = (avpCode >> 8) & 0xFF;
    msg[offset + 3] = avpCode & 0xFF;
    // Flags: Mandatory (0x40)
    msg[offset + 4] = 0x40;
    // AVP Length (3 bytes)
    msg[offset + 5] = (avpTotalLen >> 16) & 0xFF;
    msg[offset + 6] = (avpTotalLen >> 8) & 0xFF;
    msg[offset + 7] = avpTotalLen & 0xFF;
    // Data
    std::memcpy(&msg[offset + 8], originHost.data(), originHost.size());

    return msg;
}

/**
 * Extract the Result-Code value from a Diameter answer (scan AVPs).
 * Returns 0 if not found.
 */
uint32_t extractResultCode(const Buffer& msg) {
    if (msg.size() < HEADER_SIZE) return 0;

    size_t pos = HEADER_SIZE;
    while (pos + 8 <= msg.size()) {
        uint32_t avpCode = (static_cast<uint32_t>(msg[pos]) << 24) | (static_cast<uint32_t>(msg[pos + 1]) << 16) |
                           (static_cast<uint32_t>(msg[pos + 2]) << 8) | static_cast<uint32_t>(msg[pos + 3]);
        uint32_t avpLen = (static_cast<uint32_t>(msg[pos + 5]) << 16) | (static_cast<uint32_t>(msg[pos + 6]) << 8) |
                          static_cast<uint32_t>(msg[pos + 7]);

        if (avpLen < 8) break;  // malformed

        if (avpCode == 268 && avpLen >= 12) {  // Result-Code
            uint32_t rc = (static_cast<uint32_t>(msg[pos + 8]) << 24) | (static_cast<uint32_t>(msg[pos + 9]) << 16) |
                          (static_cast<uint32_t>(msg[pos + 10]) << 8) | static_cast<uint32_t>(msg[pos + 11]);
            return rc;
        }

        // Advance to next AVP (padded to 4 bytes)
        pos += (avpLen + 3) & ~3u;
    }
    return 0;
}

/**
 * Hex dump of a buffer (for debugging).
 */
void hexDump(const Buffer& buf, size_t maxBytes = 64) {
    size_t count = std::min(buf.size(), maxBytes);
    for (size_t i = 0; i < count; ++i) {
        std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(buf[i]);
        if ((i + 1) % 4 == 0) std::cout << " ";
    }
    if (buf.size() > maxBytes) std::cout << "...";
    std::cout << std::dec << "\n";
}

}  // anonymous namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 3868;
    uint32_t cmdCode = 999;  // arbitrary test command code

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc >= 4) cmdCode = static_cast<uint32_t>(std::atoi(argv[3]));

    std::cout << "[ping_client] Connecting to " << host << ":" << port << " command-code=" << cmdCode << "\n";

    boost::asio::io_context io;

    // Configure local peer identity
    ert::diametercomm::Peer::Config config;
    config.originHost = "ping-client.example.com";
    config.originRealm = "example.com";
    config.vendorId = 0;
    config.productName = "diametercomm-ping-client";
    config.watchdogIntervalSec = 0;  // no watchdog for a one-shot client
    config.applicationId = 0;

    ert::diametercomm::DiameterClient client(io, config);
    client.setReconnectEnabled(false);  // one-shot, no reconnect

    bool done = false;

    // State callback: send request once peer is Open
    client.setStateCallback([&](ert::diametercomm::Peer::State state) {
        if (state == ert::diametercomm::Peer::State::Open) {
            std::cout << "[ping_client] Peer is Open, sending request...\n";

            Buffer request = buildRequest(cmdCode, config.applicationId, config.originHost);
            std::cout << "[ping_client] Request (" << request.size() << " bytes): ";
            hexDump(request);

            uint32_t hbh = client.send(
                std::move(request),
                [&](const Buffer& answer) {
                    std::cout << "[ping_client] Answer received (" << answer.size() << " bytes): ";
                    hexDump(answer);

                    uint32_t rc = extractResultCode(answer);
                    if (rc) {
                        std::cout << "[ping_client] Result-Code: " << rc << "\n";
                    }

                    done = true;
                    client.close();
                },
                5000  // 5s timeout
            );

            if (hbh == 0) {
                std::cerr << "[ping_client] ERROR: send failed\n";
                done = true;
                io.stop();
            } else {
                std::cout << "[ping_client] Sent (hop-by-hop=" << hbh << ")\n";
            }
        } else if (state == ert::diametercomm::Peer::State::Closed) {
            if (!done) {
                std::cerr << "[ping_client] Connection closed unexpectedly\n";
                io.stop();
            }
        }
    });

    // Timeout callback
    client.setTimeoutCallback([&](uint32_t hbh) {
        std::cerr << "[ping_client] TIMEOUT waiting for answer (hop-by-hop=" << hbh << ")\n";
        done = true;
        client.close();
    });

    client.connect(host, port);
    io.run();

    std::cout << "[ping_client] Done.\n";
    return done ? 0 : 1;
}
