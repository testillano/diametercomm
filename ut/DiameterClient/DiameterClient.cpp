#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <ert/diametercomm/DiameterClient.hpp>
#include <ert/diametercomm/DiameterServer.hpp>

using namespace ert::diametercomm;

// SCTP may be unavailable in some CI/container kernels; skip SCTP tests there.
static bool sctpAvailable() {
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

class DiameterClient_test : public ::testing::Test {
   protected:
    boost::asio::io_context io_;

    Peer::Config serverConfig() {
        return {"server.example.com", "example.com", "127.0.0.1", 0, "TestServer", 0, {16777238}};
    }

    Peer::Config clientConfig() {
        return {"client.example.com", "example.com", "127.0.0.1", 0, "TestClient", 0, {16777238}};
    }

    void runFor(std::chrono::milliseconds timeout) {
        io_.restart();
        io_.run_for(timeout);
    }

    // Build a minimal application request (command code 272, R-bit set)
    static Peer::Buffer buildAppRequest(uint32_t hbh = 0) {
        Peer::Buffer msg(20, 0);
        msg[0] = 1;
        msg[1] = 0;
        msg[2] = 0;
        msg[3] = 20;
        msg[4] = 0x80;  // R-bit
        msg[5] = 0;
        msg[6] = 1;
        msg[7] = 0x10;  // 272
        msg[8] = 0;
        msg[9] = 0;
        msg[10] = 0;
        msg[11] = 4;
        msg[12] = static_cast<uint8_t>(hbh >> 24);
        msg[13] = static_cast<uint8_t>(hbh >> 16);
        msg[14] = static_cast<uint8_t>(hbh >> 8);
        msg[15] = static_cast<uint8_t>(hbh);
        return msg;
    }

    // Build a minimal answer (same as request but clear R-bit, copy hbh)
    static Peer::Buffer buildAppAnswer(uint32_t hbh) {
        Peer::Buffer msg(20, 0);
        msg[0] = 1;
        msg[1] = 0;
        msg[2] = 0;
        msg[3] = 20;
        msg[4] = 0x00;  // no R-bit = answer
        msg[5] = 0;
        msg[6] = 1;
        msg[7] = 0x10;  // 272
        msg[8] = 0;
        msg[9] = 0;
        msg[10] = 0;
        msg[11] = 4;
        msg[12] = static_cast<uint8_t>(hbh >> 24);
        msg[13] = static_cast<uint8_t>(hbh >> 16);
        msg[14] = static_cast<uint8_t>(hbh >> 8);
        msg[15] = static_cast<uint8_t>(hbh);
        return msg;
    }

    static uint32_t extractHopByHop(const Peer::Buffer& msg) {
        return (uint32_t(msg[12]) << 24) | (uint32_t(msg[13]) << 16) | (uint32_t(msg[14]) << 8) | uint32_t(msg[15]);
    }
};

TEST_F(DiameterClient_test, ConnectAndBecomeReady) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14868);

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);

    std::atomic<bool> ready{false};
    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) ready = true;
    });

    client.connect("127.0.0.1", 14868);
    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(ready);
    EXPECT_TRUE(client.isReady());
    EXPECT_EQ(client.state(), Peer::State::Open);

    client.close();
    server.close();
}

TEST_F(DiameterClient_test, SendRequestAndReceiveCorrelatedResponse) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14869);

    // Server echoes back an answer for every request
    server.setRequestCallback([&](std::shared_ptr<Peer> peer, Peer::Buffer&& msg) {
        uint32_t hbh = extractHopByHop(msg);
        auto answer = buildAppAnswer(hbh);
        peer->send(std::move(answer));
    });

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);

    Peer::Buffer responseReceived;
    std::atomic<bool> gotResponse{false};

    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) {
            auto req = buildAppRequest();
            client.send(
                std::move(req),
                [&](const Peer::Buffer& resp) {
                    responseReceived = resp;
                    gotResponse = true;
                },
                5000);
        }
    });

    client.connect("127.0.0.1", 14869);
    runFor(std::chrono::milliseconds(500));

    ASSERT_TRUE(gotResponse);
    ASSERT_GE(responseReceived.size(), 20u);
    // Should be an answer (R-bit clear)
    EXPECT_EQ(responseReceived[4] & 0x80, 0);

    client.close();
    server.close();
}

TEST_F(DiameterClient_test, SendFailsWhenNotConnected) {
    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);

    auto req = buildAppRequest();
    uint32_t hbh = client.send(std::move(req), [](const Peer::Buffer&) {}, 1000);
    EXPECT_EQ(hbh, 0u);  // send should fail
}

TEST_F(DiameterClient_test, TimeoutTriggered) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14870);

    // Server does NOT respond — simulates timeout
    server.setRequestCallback([](std::shared_ptr<Peer>, Peer::Buffer&&) {});

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);

    std::atomic<bool> timedOut{false};
    client.setTimeoutCallback([&](uint32_t) { timedOut = true; });

    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) {
            auto req = buildAppRequest();
            client.send(
                std::move(req), [](const Peer::Buffer&) { FAIL() << "Should not receive response"; },
                200);  // 200ms timeout
        }
    });

    client.connect("127.0.0.1", 14870);
    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(timedOut);

    client.close();
    server.close();
}

TEST_F(DiameterClient_test, GracefulDisconnect) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14871);

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);

    std::atomic<bool> closed{false};
    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) {
            client.disconnect(0);
        } else if (s == Peer::State::Closed) {
            closed = true;
        }
    });

    client.connect("127.0.0.1", 14871);
    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(closed);
    EXPECT_EQ(client.state(), Peer::State::Closed);

    server.close();
}

TEST_F(DiameterClient_test, ReconnectOnConnectionLoss) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14872);

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(true);
    client.setReconnectBackoff(std::chrono::milliseconds(100), std::chrono::milliseconds(500));

    std::atomic<int> openCount{0};
    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) openCount++;
    });

    client.connect("127.0.0.1", 14872);
    runFor(std::chrono::milliseconds(300));
    EXPECT_EQ(openCount.load(), 1);

    // Force server to close the connection
    server.close();
    runFor(std::chrono::milliseconds(100));

    // Restart server on same port
    DiameterServer server2(io_, serverConfig());
    server2.listen("127.0.0.1", 14872);

    // Wait for reconnect (100ms backoff + connection time)
    runFor(std::chrono::milliseconds(500));
    EXPECT_GE(openCount.load(), 2);

    client.close();
    server2.close();
}

TEST_F(DiameterClient_test, UnsolicitedRequestFromServer) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14873);

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);

    Peer::Buffer unsolicited;
    std::atomic<bool> received{false};
    client.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&& msg) {
        unsolicited = std::move(msg);
        received = true;
    });

    // Once server has a peer, send an unsolicited request to the client
    server.setPeerEventCallback([&](std::shared_ptr<Peer> peer, Peer::State s) {
        if (s == Peer::State::Open) {
            auto req = buildAppRequest(0xABCD);
            peer->send(std::move(req));
        }
    });

    client.connect("127.0.0.1", 14873);
    runFor(std::chrono::milliseconds(500));

    ASSERT_TRUE(received);
    ASSERT_GE(unsolicited.size(), 20u);
    EXPECT_NE(extractHopByHop(unsolicited), 0u);

    client.close();
    server.close();
}

// =============================================================================
// SCTP single-homing (client transport). Exercises the DiameterClient ->
// Peer -> PeerConnection SCTP plumbing (G1) against an SCTP DiameterServer.
// =============================================================================

TEST_F(DiameterClient_test, ConnectAndBecomeReady_SCTP) {
    if (!sctpAvailable()) GTEST_SKIP() << "SCTP not available in this environment";

    DiameterServer server(io_, serverConfig(), Transport::SCTP);
    server.listen("127.0.0.1", 14880);

    DiameterClient client(io_, clientConfig(), Transport::SCTP);
    client.setReconnectEnabled(false);

    std::atomic<bool> ready{false};
    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) ready = true;
    });

    client.connect("127.0.0.1", 14880);
    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(ready);
    EXPECT_TRUE(client.isReady());
    EXPECT_EQ(client.state(), Peer::State::Open);

    client.close();
    server.close();
}

TEST_F(DiameterClient_test, SendRequestAndReceiveCorrelatedResponse_SCTP) {
    if (!sctpAvailable()) GTEST_SKIP() << "SCTP not available in this environment";

    DiameterServer server(io_, serverConfig(), Transport::SCTP);
    server.listen("127.0.0.1", 14881);

    // Server echoes back an answer for every request
    server.setRequestCallback([&](std::shared_ptr<Peer> peer, Peer::Buffer&& msg) {
        uint32_t hbh = extractHopByHop(msg);
        peer->send(buildAppAnswer(hbh));
    });

    DiameterClient client(io_, clientConfig(), Transport::SCTP);
    client.setReconnectEnabled(false);

    Peer::Buffer responseReceived;
    std::atomic<bool> gotResponse{false};

    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) {
            client.send(
                buildAppRequest(),
                [&](const Peer::Buffer& resp) {
                    responseReceived = resp;
                    gotResponse = true;
                },
                5000);
        }
    });

    client.connect("127.0.0.1", 14881);
    runFor(std::chrono::milliseconds(500));

    ASSERT_TRUE(gotResponse);
    ASSERT_GE(responseReceived.size(), 20u);
    EXPECT_EQ(responseReceived[4] & 0x80, 0);  // answer (R-bit clear)

    client.close();
    server.close();
}

// Backwards-compat guard: default transport must remain TCP, so an SCTP server
// and a default (TCP) client must NOT establish an Open association.
TEST_F(DiameterClient_test, DefaultTransportIsTcp_NoMixedAssociation) {
    if (!sctpAvailable()) GTEST_SKIP() << "SCTP not available in this environment";

    DiameterServer server(io_, serverConfig(), Transport::SCTP);
    server.listen("127.0.0.1", 14882);

    DiameterClient client(io_, clientConfig());  // no transport arg -> TCP
    client.setReconnectEnabled(false);

    std::atomic<bool> ready{false};
    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) ready = true;
    });

    client.connect("127.0.0.1", 14882);
    runFor(std::chrono::milliseconds(400));

    EXPECT_FALSE(ready);
    EXPECT_FALSE(client.isReady());

    client.close();
    server.close();
}

// Reconnect must reuse the configured SCTP transport: after losing the SCTP
// association, the client re-establishes a brand-new SCTP association (INIT/
// COOKIE + CER/CEA) against a server relistening on the same port. Mirrors the
// TCP ReconnectOnConnectionLoss test.
TEST_F(DiameterClient_test, ReconnectOnConnectionLoss_SCTP) {
    if (!sctpAvailable()) GTEST_SKIP() << "SCTP not available in this environment";

    DiameterServer server(io_, serverConfig(), Transport::SCTP);
    server.listen("127.0.0.1", 14883);

    DiameterClient client(io_, clientConfig(), Transport::SCTP);
    client.setReconnectEnabled(true);
    client.setReconnectBackoff(std::chrono::milliseconds(100), std::chrono::milliseconds(500));

    std::atomic<int> openCount{0};
    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) openCount++;
    });

    client.connect("127.0.0.1", 14883);
    runFor(std::chrono::milliseconds(300));
    EXPECT_EQ(openCount.load(), 1);

    // Force server to close the association
    server.close();
    runFor(std::chrono::milliseconds(100));

    // Restart an SCTP server on the same port
    DiameterServer server2(io_, serverConfig(), Transport::SCTP);
    server2.listen("127.0.0.1", 14883);

    // Wait for reconnect (100ms backoff + SCTP handshake + CER/CEA)
    runFor(std::chrono::milliseconds(600));
    EXPECT_GE(openCount.load(), 2);

    client.close();
    server2.close();
}
