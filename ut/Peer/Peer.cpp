#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ert/diametercomm/Peer.hpp>
#include <thread>

using namespace ert::diametercomm;

// ============================================================================
// Test fixture: loopback server + client peer exchange
// ============================================================================
class Peer_test : public ::testing::Test {
   protected:
    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    uint16_t port_{0};

    Peer::Config serverConfig() {
        return {"server.example.com",       "example.com", "127.0.0.1", 0, "TestServer",
                0 /*no watchdog in tests*/, {16777238}};
    }

    Peer::Config clientConfig() {
        return {"client.example.com",       "example.com", "127.0.0.1", 0, "TestClient",
                0 /*no watchdog in tests*/, {16777238}};
    }

    void SetUp() override {
        acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(
            io_, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
        port_ = acceptor_->local_endpoint().port();
    }

    void TearDown() override { acceptor_->close(); }

    void runFor(std::chrono::milliseconds timeout) {
        io_.restart();
        io_.run_for(timeout);
    }

    // Helper: build a minimal non-base-protocol request (command code 272 = CCR)
    static Peer::Buffer buildAppRequest(uint32_t hbh = 0, uint32_t e2e = 0) {
        Peer::Buffer msg(20, 0);
        msg[0] = 1;
        msg[1] = 0;
        msg[2] = 0;
        msg[3] = 20;
        msg[4] = 0x80;  // R-bit
        // command code = 272 (Credit-Control)
        msg[5] = 0;
        msg[6] = 1;
        msg[7] = 0x10;
        // app-id = 4 (Credit-Control)
        msg[8] = 0;
        msg[9] = 0;
        msg[10] = 0;
        msg[11] = 4;
        // hbh
        msg[12] = static_cast<uint8_t>(hbh >> 24);
        msg[13] = static_cast<uint8_t>(hbh >> 16);
        msg[14] = static_cast<uint8_t>(hbh >> 8);
        msg[15] = static_cast<uint8_t>(hbh);
        // e2e
        msg[16] = static_cast<uint8_t>(e2e >> 24);
        msg[17] = static_cast<uint8_t>(e2e >> 16);
        msg[18] = static_cast<uint8_t>(e2e >> 8);
        msg[19] = static_cast<uint8_t>(e2e);
        return msg;
    }
};

// ============================================================================
// Test: CER/CEA exchange (client connects, sends CER, server responds CEA)
// ============================================================================
TEST_F(Peer_test, CerCeaExchange) {
    std::atomic<bool> clientOpen{false};
    std::atomic<bool> serverOpen{false};

    auto serverPeer = std::make_shared<Peer>(std::make_shared<PeerConnection>(boost::asio::ip::tcp::socket(io_)), io_,
                                             serverConfig());

    acceptor_->async_accept(serverPeer->socket(), [&](const boost::system::error_code& ec) {
        ASSERT_FALSE(ec) << ec.message();
        serverPeer->setStateCallback([&](std::shared_ptr<Peer>, Peer::State s) {
            if (s == Peer::State::Open) serverOpen = true;
        });
        serverPeer->start();
    });

    auto clientPeer = std::make_shared<Peer>(io_, clientConfig());
    clientPeer->setStateCallback([&](std::shared_ptr<Peer>, Peer::State s) {
        if (s == Peer::State::Open) clientOpen = true;
    });
    clientPeer->connect("127.0.0.1", port_);

    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(clientOpen);
    EXPECT_TRUE(serverOpen);
    EXPECT_EQ(clientPeer->state(), Peer::State::Open);
    EXPECT_EQ(serverPeer->state(), Peer::State::Open);

    // Verify origin host extracted
    EXPECT_EQ(clientPeer->remoteOriginHost(), "server.example.com");
    EXPECT_EQ(serverPeer->remoteOriginHost(), "client.example.com");
    EXPECT_EQ(clientPeer->remoteOriginRealm(), "example.com");
    EXPECT_EQ(serverPeer->remoteOriginRealm(), "example.com");
}

// ============================================================================
// Test: Application message delivery after CER/CEA
// ============================================================================
TEST_F(Peer_test, ApplicationMessageDelivery) {
    Peer::Buffer receivedMsg;
    std::atomic<bool> received{false};

    auto serverPeer = std::make_shared<Peer>(std::make_shared<PeerConnection>(boost::asio::ip::tcp::socket(io_)), io_,
                                             serverConfig());

    serverPeer->setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&& msg) {
        receivedMsg = std::move(msg);
        received = true;
    });

    acceptor_->async_accept(serverPeer->socket(), [&](const boost::system::error_code& ec) {
        ASSERT_FALSE(ec);
        serverPeer->start();
    });

    auto clientPeer = std::make_shared<Peer>(io_, clientConfig());
    clientPeer->setStateCallback([&](std::shared_ptr<Peer> p, Peer::State s) {
        if (s == Peer::State::Open) {
            // Send application message once open
            auto appMsg = buildAppRequest();
            p->send(std::move(appMsg));
        }
    });
    clientPeer->connect("127.0.0.1", port_);

    runFor(std::chrono::milliseconds(500));

    ASSERT_TRUE(received);
    ASSERT_GE(receivedMsg.size(), 20u);
    // Command code should be 272
    uint32_t cmdCode = (uint32_t(receivedMsg[5]) << 16) | (uint32_t(receivedMsg[6]) << 8) | uint32_t(receivedMsg[7]);
    EXPECT_EQ(cmdCode, 272u);
    // Hop-by-hop should have been auto-assigned (non-zero)
    uint32_t hbh = (uint32_t(receivedMsg[12]) << 24) | (uint32_t(receivedMsg[13]) << 16) |
                   (uint32_t(receivedMsg[14]) << 8) | uint32_t(receivedMsg[15]);
    EXPECT_NE(hbh, 0u);
}

// ============================================================================
// Test: send() returns false when not Open
// ============================================================================
TEST_F(Peer_test, SendFailsWhenNotOpen) {
    auto peer = std::make_shared<Peer>(io_, clientConfig());
    auto msg = buildAppRequest(1, 1);
    EXPECT_FALSE(peer->send(std::move(msg)));
}

// ============================================================================
// Test: DPR/DPA graceful disconnect
// ============================================================================
TEST_F(Peer_test, GracefulDisconnect) {
    std::atomic<bool> clientClosed{false};
    std::atomic<bool> serverClosed{false};

    auto serverPeer = std::make_shared<Peer>(std::make_shared<PeerConnection>(boost::asio::ip::tcp::socket(io_)), io_,
                                             serverConfig());

    serverPeer->setStateCallback([&](std::shared_ptr<Peer>, Peer::State s) {
        if (s == Peer::State::Closed) serverClosed = true;
    });

    acceptor_->async_accept(serverPeer->socket(), [&](const boost::system::error_code& ec) {
        ASSERT_FALSE(ec);
        serverPeer->start();
    });

    auto clientPeer = std::make_shared<Peer>(io_, clientConfig());
    clientPeer->setStateCallback([&](std::shared_ptr<Peer> p, Peer::State s) {
        if (s == Peer::State::Open) {
            // Immediately disconnect
            p->disconnect(0);  // REBOOTING
        } else if (s == Peer::State::Closed) {
            clientClosed = true;
        }
    });
    clientPeer->connect("127.0.0.1", port_);

    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(clientClosed);
    EXPECT_TRUE(serverClosed);
    EXPECT_EQ(clientPeer->state(), Peer::State::Closed);
}

// ============================================================================
// Test: DWR/DWA watchdog exchange
// ============================================================================
TEST_F(Peer_test, WatchdogExchange) {
    // Use 1-second watchdog for test
    auto sConfig = serverConfig();
    sConfig.watchdogIntervalSec = 1;
    auto cConfig = clientConfig();
    cConfig.watchdogIntervalSec = 0;  // only server sends DWR

    std::atomic<bool> bothOpen{false};

    auto serverPeer =
        std::make_shared<Peer>(std::make_shared<PeerConnection>(boost::asio::ip::tcp::socket(io_)), io_, sConfig);

    acceptor_->async_accept(serverPeer->socket(), [&](const boost::system::error_code& ec) {
        ASSERT_FALSE(ec);
        serverPeer->start();
    });

    auto clientPeer = std::make_shared<Peer>(io_, cConfig);
    clientPeer->setStateCallback([&](std::shared_ptr<Peer>, Peer::State s) {
        if (s == Peer::State::Open) bothOpen = true;
    });
    clientPeer->connect("127.0.0.1", port_);

    // Run for 1.5 seconds to allow at least one DWR/DWA exchange
    runFor(std::chrono::milliseconds(1500));

    EXPECT_TRUE(bothOpen);
    // Both should still be open (watchdog keeps connection alive)
    EXPECT_EQ(serverPeer->state(), Peer::State::Open);
    EXPECT_EQ(clientPeer->state(), Peer::State::Open);

    // Cleanup
    serverPeer->close();
    clientPeer->close();
}

// ============================================================================
// Test: state transitions (Closed -> WaitCEA -> Open)
// ============================================================================
TEST_F(Peer_test, StateTransitionsClient) {
    std::vector<Peer::State> states;

    auto serverPeer = std::make_shared<Peer>(std::make_shared<PeerConnection>(boost::asio::ip::tcp::socket(io_)), io_,
                                             serverConfig());

    acceptor_->async_accept(serverPeer->socket(), [&](const boost::system::error_code& ec) {
        ASSERT_FALSE(ec);
        serverPeer->start();
    });

    auto clientPeer = std::make_shared<Peer>(io_, clientConfig());
    clientPeer->setStateCallback([&](std::shared_ptr<Peer>, Peer::State s) { states.push_back(s); });
    clientPeer->connect("127.0.0.1", port_);

    runFor(std::chrono::milliseconds(500));

    // Should see: WaitCEA, Open
    ASSERT_GE(states.size(), 2u);
    EXPECT_EQ(states[0], Peer::State::WaitCEA);
    EXPECT_EQ(states[1], Peer::State::Open);
}

// ============================================================================
// Test: server state transitions (Closed -> WaitCER -> Open)
// ============================================================================
TEST_F(Peer_test, StateTransitionsServer) {
    std::vector<Peer::State> states;

    auto serverPeer = std::make_shared<Peer>(std::make_shared<PeerConnection>(boost::asio::ip::tcp::socket(io_)), io_,
                                             serverConfig());

    serverPeer->setStateCallback([&](std::shared_ptr<Peer>, Peer::State s) { states.push_back(s); });

    acceptor_->async_accept(serverPeer->socket(), [&](const boost::system::error_code& ec) {
        ASSERT_FALSE(ec);
        serverPeer->start();
    });

    auto clientPeer = std::make_shared<Peer>(io_, clientConfig());
    clientPeer->connect("127.0.0.1", port_);

    runFor(std::chrono::milliseconds(500));

    // Should see: WaitCER, Open
    ASSERT_GE(states.size(), 2u);
    EXPECT_EQ(states[0], Peer::State::WaitCER);
    EXPECT_EQ(states[1], Peer::State::Open);
}

// ============================================================================
// Test: hop-by-hop auto-assignment
// ============================================================================
TEST_F(Peer_test, HopByHopAutoAssignment) {
    auto peer = std::make_shared<Peer>(io_, clientConfig());

    uint32_t first = peer->nextHopByHop();
    uint32_t second = peer->nextHopByHop();
    uint32_t third = peer->nextHopByHop();

    EXPECT_EQ(second, first + 1);
    EXPECT_EQ(third, first + 2);
}

// ============================================================================
// Test: force close
// ============================================================================
TEST_F(Peer_test, ForceClose) {
    std::atomic<bool> closed{false};

    auto serverPeer = std::make_shared<Peer>(std::make_shared<PeerConnection>(boost::asio::ip::tcp::socket(io_)), io_,
                                             serverConfig());

    acceptor_->async_accept(serverPeer->socket(), [&](const boost::system::error_code& ec) {
        ASSERT_FALSE(ec);
        serverPeer->start();
    });

    auto clientPeer = std::make_shared<Peer>(io_, clientConfig());
    clientPeer->setStateCallback([&](std::shared_ptr<Peer> p, Peer::State s) {
        if (s == Peer::State::Open) {
            p->close();  // force close, no DPR
        } else if (s == Peer::State::Closed) {
            closed = true;
        }
    });
    clientPeer->connect("127.0.0.1", port_);

    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(closed);
    EXPECT_EQ(clientPeer->state(), Peer::State::Closed);
}

// ============================================================================
// Identifier semantics (RFC 6733):
//  - Hop-by-Hop is PER-CONNECTION: each Peer has its own randomly-seeded
//    counter, independent from other Peers.
//  - End-to-End is NODE-WIDE: a single per-process sequence shared by every
//    Peer, seeded once at startup and monotonically increasing.
// ============================================================================

TEST_F(Peer_test, EndToEndIsNodeWideMonotonic) {
    // Two independent peers (distinct "connections") must draw from the same
    // node-wide End-to-End sequence, so consecutive draws increase by one
    // regardless of which peer issued them.
    Peer a(io_, clientConfig());
    Peer b(io_, serverConfig());

    uint32_t e1 = a.nextEndToEnd();
    uint32_t e2 = b.nextEndToEnd();  // different peer, same node sequence
    uint32_t e3 = a.nextEndToEnd();

    EXPECT_EQ(e2, e1 + 1);
    EXPECT_EQ(e3, e2 + 1);
}

TEST_F(Peer_test, HopByHopIsPerConnection) {
    Peer a(io_, clientConfig());
    Peer b(io_, clientConfig());

    // Each peer increments its own Hop-by-Hop sequence independently.
    uint32_t a1 = a.nextHopByHop();
    uint32_t b1 = b.nextHopByHop();
    uint32_t a2 = a.nextHopByHop();
    uint32_t b2 = b.nextHopByHop();

    EXPECT_EQ(a2, a1 + 1);  // a advances on its own counter
    EXPECT_EQ(b2, b1 + 1);  // b advances on its own counter
    // Interleaving a and b did not perturb each other's sequence: this only
    // holds if the counters are per-Peer (not shared like End-to-End).
}

// ============================================================================
// Test: extra CER / CEA AVPs are appended verbatim after the built ones
// (one side is a raw socket, to read the base protocol message as it travels)
// ============================================================================
namespace {
// Supported-Vendor-Id (265) = 10415, flags 0: 8-byte header + 4-byte value
const std::vector<uint8_t> SupportedVendorId10415 = {0, 0, 1, 9, 0, 0, 0, 12, 0, 0, 0x28, 0xaf};
// Firmware-Revision (267) = 7
const std::vector<uint8_t> FirmwareRevision7 = {0, 0, 1, 11, 0, 0, 0, 12, 0, 0, 0, 7};

// Whole message read from a raw socket (4-byte header gives the length).
Peer::Buffer readMessage(boost::asio::ip::tcp::socket& s) {
    Peer::Buffer msg(4);
    boost::asio::read(s, boost::asio::buffer(msg));
    const std::size_t len = (std::size_t(msg[1]) << 16) | (std::size_t(msg[2]) << 8) | msg[3];
    msg.resize(len);
    boost::asio::read(s, boost::asio::buffer(msg.data() + 4, len - 4));
    return msg;
}

// Codes of the AVPs of a message (top level), in order.
std::vector<uint32_t> avpCodes(const Peer::Buffer& msg) {
    std::vector<uint32_t> codes;
    std::size_t pos = 20;
    while (pos + 8 <= msg.size()) {
        codes.push_back((uint32_t(msg[pos]) << 24) | (uint32_t(msg[pos + 1]) << 16) | (uint32_t(msg[pos + 2]) << 8) |
                        msg[pos + 3]);
        std::size_t len = (std::size_t(msg[pos + 5]) << 16) | (std::size_t(msg[pos + 6]) << 8) | msg[pos + 7];
        pos += (len + 3) & ~std::size_t(3);
    }
    return codes;
}

bool endsWith(const Peer::Buffer& msg, const std::vector<uint8_t>& tail) {
    return msg.size() >= tail.size() && std::equal(tail.begin(), tail.end(), msg.end() - tail.size());
}
}  // namespace

TEST_F(Peer_test, ExtraCerAvps) {
    auto config = clientConfig();
    config.extraCerAvps = SupportedVendorId10415;
    config.extraCerAvps.insert(config.extraCerAvps.end(), FirmwareRevision7.begin(), FirmwareRevision7.end());

    boost::asio::ip::tcp::socket raw(io_);
    acceptor_->async_accept(raw, [](const boost::system::error_code&) {});
    auto clientPeer = std::make_shared<Peer>(io_, config);
    clientPeer->connect("127.0.0.1", port_);
    runFor(std::chrono::milliseconds(300));
    ASSERT_TRUE(raw.is_open());

    const Peer::Buffer cer = readMessage(raw);
    EXPECT_EQ(cer[4] & 0x80, 0x80);  // request
    EXPECT_EQ((uint32_t(cer[5]) << 16) | (uint32_t(cer[6]) << 8) | cer[7], 257u);
    // built AVPs first (Origin-Host, Origin-Realm, Host-IP-Address, Vendor-Id, Product-Name,
    // Auth-Application-Id), then the extra ones, in order
    EXPECT_EQ(avpCodes(cer), (std::vector<uint32_t>{264, 296, 257, 266, 269, 258, 265, 267}));
    EXPECT_TRUE(endsWith(cer, FirmwareRevision7));
    clientPeer->close();
}

TEST_F(Peer_test, ExtraCeaAvpsAndNoneByDefault) {
    // default: no extra AVP in the CER
    {
        boost::asio::ip::tcp::socket raw(io_);
        acceptor_->async_accept(raw, [](const boost::system::error_code&) {});
        auto clientPeer = std::make_shared<Peer>(io_, clientConfig());
        clientPeer->connect("127.0.0.1", port_);
        runFor(std::chrono::milliseconds(300));
        ASSERT_TRUE(raw.is_open());
        EXPECT_EQ(avpCodes(readMessage(raw)), (std::vector<uint32_t>{264, 296, 257, 266, 269, 258}));
        clientPeer->close();
    }

    // CEA: a server peer with extra AVPs answers the CER of a client peer; the raw side
    // is the client, so take the CER of a client Peer through a second raw acceptor
    auto config = serverConfig();
    config.extraCeaAvps = SupportedVendorId10415;
    auto serverPeer =
        std::make_shared<Peer>(std::make_shared<PeerConnection>(boost::asio::ip::tcp::socket(io_)), io_, config);
    acceptor_->async_accept(serverPeer->socket(), [&](const boost::system::error_code& ec) {
        if (!ec) serverPeer->start();
    });

    boost::asio::ip::tcp::acceptor cerSource(io_, {boost::asio::ip::make_address("127.0.0.1"), 0});
    boost::asio::ip::tcp::socket cerRaw(io_);
    cerSource.async_accept(cerRaw, [](const boost::system::error_code&) {});
    auto cerPeer = std::make_shared<Peer>(io_, clientConfig());
    cerPeer->connect("127.0.0.1", cerSource.local_endpoint().port());

    boost::asio::ip::tcp::socket raw(io_);
    raw.async_connect({boost::asio::ip::make_address("127.0.0.1"), port_}, [](const boost::system::error_code&) {});
    runFor(std::chrono::milliseconds(300));
    ASSERT_TRUE(cerRaw.is_open());
    const Peer::Buffer cer = readMessage(cerRaw);
    boost::asio::write(raw, boost::asio::buffer(cer));
    runFor(std::chrono::milliseconds(300));

    const Peer::Buffer cea = readMessage(raw);
    EXPECT_EQ(cea[4] & 0x80, 0);  // answer
    EXPECT_EQ((uint32_t(cea[5]) << 16) | (uint32_t(cea[6]) << 8) | cea[7], 257u);
    EXPECT_EQ(avpCodes(cea), (std::vector<uint32_t>{268, 264, 296, 257, 266, 269, 258, 265}));
    EXPECT_TRUE(endsWith(cea, SupportedVendorId10415));
    EXPECT_EQ(serverPeer->state(), Peer::State::Open);
    cerPeer->close();
    serverPeer->close();
}
