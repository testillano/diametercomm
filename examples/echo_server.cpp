/*
 ___________________________________________________________________________
|                                                                           |
|   diametercomm example: echo_server                                       |
|                                                                           |
|   Listens for incoming Diameter peer connections and echoes back every     |
|   application-level request as an answer (clears Request flag, sets       |
|   Result-Code AVP = DIAMETER_SUCCESS 2001).                               |
|                                                                           |
|   Usage:                                                                  |
|     echo_server [bind_address] [port]                                     |
|     echo_server                       # 0.0.0.0:3868                      |
|     echo_server 127.0.0.1 13868                                           |
|___________________________________________________________________________|
*/

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <boost/asio.hpp>

#include <ert/diametercomm/DiameterServer.hpp>
#include <ert/diametercomm/Peer.hpp>

namespace {

// Diameter header constants (RFC 6733 section 3)
constexpr uint32_t FLAG_REQUEST = 0x80;
constexpr size_t HEADER_SIZE = 20;
constexpr size_t FLAGS_OFFSET = 4;

// AVP: Result-Code (268), mandatory, uint32
// AVP header: code(4) + flags(1) + length(3) = 8 bytes, payload = 4 bytes
constexpr uint32_t AVP_RESULT_CODE = 268;
constexpr uint32_t DIAMETER_SUCCESS = 2001;

using Buffer = ert::diametercomm::Peer::Buffer;

/**
 * Build a Diameter answer from a request.
 * - Copies the request verbatim
 * - Clears the Request flag (R-bit)
 * - Appends a Result-Code AVP with DIAMETER_SUCCESS
 * - Updates the message length in the header
 */
Buffer buildAnswer(const Buffer& request) {
    if (request.size() < HEADER_SIZE) {
        return {}; // malformed
    }

    // Start with a copy of the request
    Buffer answer(request);

    // Clear the Request flag (bit 7 of the flags byte)
    answer[FLAGS_OFFSET] &= ~FLAG_REQUEST;

    // Build Result-Code AVP (268)
    // AVP header: Code(4) + Flags(1) + Length(3) = 8; payload: uint32(4) = total 12
    uint8_t avp[12];
    std::memset(avp, 0, sizeof(avp));

    // AVP Code = 268 (network byte order)
    avp[0] = (AVP_RESULT_CODE >> 24) & 0xFF;
    avp[1] = (AVP_RESULT_CODE >> 16) & 0xFF;
    avp[2] = (AVP_RESULT_CODE >> 8) & 0xFF;
    avp[3] = AVP_RESULT_CODE & 0xFF;

    // AVP Flags = 0x40 (Mandatory)
    avp[4] = 0x40;

    // AVP Length = 12 (8 header + 4 data), 3 bytes
    avp[5] = 0x00;
    avp[6] = 0x00;
    avp[7] = 0x0C;

    // Result-Code value = 2001 (network byte order)
    avp[8] = (DIAMETER_SUCCESS >> 24) & 0xFF;
    avp[9] = (DIAMETER_SUCCESS >> 16) & 0xFF;
    avp[10] = (DIAMETER_SUCCESS >> 8) & 0xFF;
    avp[11] = DIAMETER_SUCCESS & 0xFF;

    answer.insert(answer.end(), avp, avp + sizeof(avp));

    // Update message length in header (bytes 1-3, version in byte 0)
    uint32_t newLen = static_cast<uint32_t>(answer.size());
    answer[1] = (newLen >> 16) & 0xFF;
    answer[2] = (newLen >> 8) & 0xFF;
    answer[3] = newLen & 0xFF;

    return answer;
}

/**
 * Extract command code from a Diameter message header.
 */
uint32_t commandCode(const Buffer& msg) {
    if (msg.size() < HEADER_SIZE) return 0;
    return (static_cast<uint32_t>(msg[5]) << 16) |
           (static_cast<uint32_t>(msg[6]) << 8) |
           static_cast<uint32_t>(msg[7]);
}

boost::asio::io_context* g_io = nullptr;

void signalHandler(int /*sig*/) {
    std::cout << "\n[echo_server] Shutting down...\n";
    if (g_io) {
        g_io->stop();
    }
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    std::string bindAddr = "0.0.0.0";
    uint16_t port = 3868;

    if (argc >= 2) bindAddr = argv[1];
    if (argc >= 3) port = static_cast<uint16_t>(std::atoi(argv[2]));

    std::cout << "[echo_server] Diameter echo server\n";
    std::cout << "[echo_server] Listening on " << bindAddr << ":" << port << "\n";

    boost::asio::io_context io;
    g_io = &io;

    // Register signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Configure the local peer identity
    ert::diametercomm::Peer::Config config;
    config.originHost = "echo-server.example.com";
    config.originRealm = "example.com";
    config.hostIpAddress = bindAddr;
    config.vendorId = 0;
    config.productName = "diametercomm-echo-server";
    config.watchdogIntervalSec = 30;
    config.applicationId = 0; // relay (accepts any)

    ert::diametercomm::DiameterServer server(io, config);

    // Log peer events
    server.setPeerEventCallback(
        [](std::shared_ptr<ert::diametercomm::Peer> peer,
           ert::diametercomm::Peer::State state) {
            const char* stateStr = "unknown";
            switch (state) {
                case ert::diametercomm::Peer::State::Open:
                    stateStr = "Open";
                    break;
                case ert::diametercomm::Peer::State::Closed:
                    stateStr = "Closed";
                    break;
                case ert::diametercomm::Peer::State::WaitCER:
                    stateStr = "WaitCER";
                    break;
                case ert::diametercomm::Peer::State::WaitCEA:
                    stateStr = "WaitCEA";
                    break;
                case ert::diametercomm::Peer::State::Closing:
                    stateStr = "Closing";
                    break;
            }
            std::cout << "[echo_server] Peer " << peer->remoteOriginHost()
                      << " -> " << stateStr << "\n";
        });

    // Echo every request back as an answer
    server.setRequestCallback(
        [](std::shared_ptr<ert::diametercomm::Peer> peer, Buffer&& request) {
            uint32_t cc = commandCode(request);
            std::cout << "[echo_server] Request from " << peer->remoteOriginHost()
                      << " command-code=" << cc
                      << " size=" << request.size() << " bytes\n";

            Buffer answer = buildAnswer(request);
            if (!answer.empty()) {
                peer->send(std::move(answer));
                std::cout << "[echo_server] Answer sent (Result-Code=2001)\n";
            }
        });

    server.listen(bindAddr, port);

    std::cout << "[echo_server] Running (Ctrl+C to stop)...\n";
    io.run();

    server.close();
    std::cout << "[echo_server] Done.\n";
    return 0;
}
