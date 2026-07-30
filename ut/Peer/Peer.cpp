#include <gtest/gtest.h>

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
