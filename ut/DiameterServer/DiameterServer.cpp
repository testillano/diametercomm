#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <ert/diametercomm/DiameterServer.hpp>

using namespace ert::diametercomm;

// SCTP may be unavailable in some CI/container kernels; skip SCTP tests there.
static bool sctpAvailable() {
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

class DiameterServer_test : public ::testing::Test {
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

    // Helper: build a minimal application request (command code 272 = CCR)
    static Peer::Buffer buildAppRequest() {
        Peer::Buffer msg(20, 0);
        msg[0] = 1;
        msg[1] = 0;
        msg[2] = 0;
        msg[3] = 20;
        msg[4] = 0x80;
        msg[5] = 0;
        msg[6] = 1;
        msg[7] = 0x10;
        msg[8] = 0;
        msg[9] = 0;
        msg[10] = 0;
        msg[11] = 4;
        return msg;
    }

    // Helper: build a minimal Re-Auth-Request (command code 258, R-bit set),
    // hop-by-hop optionally preset (0 lets sendRequest assign it).
    static Peer::Buffer buildRar(uint32_t hbh = 0) {
        Peer::Buffer msg(20, 0);
        msg[0] = 1;
        msg[3] = 20;
        msg[4] = 0x80;  // R-bit -> request
        msg[6] = 1;
        msg[7] = 0x02;  // 258 = RAR
        msg[11] = 4;    // application-id low byte (test appId 4)
        msg[12] = static_cast<uint8_t>(hbh >> 24);
        msg[13] = static_cast<uint8_t>(hbh >> 16);
        msg[14] = static_cast<uint8_t>(hbh >> 8);
        msg[15] = static_cast<uint8_t>(hbh);
        return msg;
    }

    // Helper: build a minimal Re-Auth-Answer (258, R-bit clear) echoing hbh.
    static Peer::Buffer buildRaa(uint32_t hbh) {
        Peer::Buffer msg(20, 0);
        msg[0] = 1;
        msg[3] = 20;
        msg[4] = 0x00;  // answer
        msg[6] = 1;
        msg[7] = 0x02;  // 258 = RAA
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
    // Answer for a received request: R-bit clear, same header (command, app,
    // e2e), hop-by-hop replaced by 'hbhOverride' when non-zero (relay rewrite).
    static Peer::Buffer answerFor(const Peer::Buffer& req, uint32_t hbhOverride = 0) {
        Peer::Buffer msg(req.begin(), req.begin() + 20);
        msg[3] = 20;
        msg[4] = 0x00;
        if (hbhOverride != 0) {
            msg[12] = static_cast<uint8_t>(hbhOverride >> 24);
            msg[13] = static_cast<uint8_t>(hbhOverride >> 16);
            msg[14] = static_cast<uint8_t>(hbhOverride >> 8);
            msg[15] = static_cast<uint8_t>(hbhOverride);
        }
        return msg;
    }
};

TEST_F(DiameterServer_test, ListenAndAcceptSinglePeer) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13868);

    std::atomic<bool> peerConnected{false};
    server.setPeerEventCallback([&](std::shared_ptr<Peer>, Peer::State s) {
        if (s == Peer::State::Open) peerConnected = true;
    });

    auto clientPeer = std::make_shared<Peer>(io_, clientConfig());
    clientPeer->connect("127.0.0.1", 13868);

    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(peerConnected);
    EXPECT_EQ(server.activePeerCount(), 1u);

    server.close();
}

TEST_F(DiameterServer_test, AcceptMultiplePeers) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13869);

    std::atomic<int> openCount{0};
    server.setPeerEventCallback([&](std::shared_ptr<Peer>, Peer::State s) {
        if (s == Peer::State::Open) openCount++;
    });

    auto client1 = std::make_shared<Peer>(io_, clientConfig());
    auto client2 = std::make_shared<Peer>(io_, clientConfig());
    client1->connect("127.0.0.1", 13869);
    client2->connect("127.0.0.1", 13869);

    runFor(std::chrono::milliseconds(500));

    EXPECT_EQ(openCount.load(), 2);
    EXPECT_EQ(server.activePeerCount(), 2u);
    EXPECT_EQ(server.peers().size(), 2u);

    server.close();
}

TEST_F(DiameterServer_test, RequestCallbackDelivery) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13870);

    Peer::Buffer receivedMsg;
    std::atomic<bool> received{false};
    server.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&& msg) {
        receivedMsg = std::move(msg);
        received = true;
    });

    auto client = std::make_shared<Peer>(io_, clientConfig());
    client->setStateCallback([&](std::shared_ptr<Peer> p, Peer::State s) {
        if (s == Peer::State::Open) {
            p->send(buildAppRequest());
        }
    });
    client->connect("127.0.0.1", 13870);

    runFor(std::chrono::milliseconds(500));

    ASSERT_TRUE(received);
    ASSERT_GE(receivedMsg.size(), 20u);
    uint32_t cmdCode = (uint32_t(receivedMsg[5]) << 16) | (uint32_t(receivedMsg[6]) << 8) | uint32_t(receivedMsg[7]);
    EXPECT_EQ(cmdCode, 272u);

    server.close();
}

// =============================================================================
// Gap B: server-initiated request (bidirectional Diameter, RFC 6733). The
// server PUSHES a request (e.g. RAR) down a connected peer and correlates the
// incoming answer (RAA) by hop-by-hop, mirroring DiameterClient::send.
// =============================================================================

TEST_F(DiameterServer_test, ServerSendsRequestAndReceivesCorrelatedAnswer) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13874);

    // The connected peer answers any request it receives with a correlated RAA.
    auto client = std::make_shared<Peer>(io_, clientConfig());
    client->setRequestCallback([&](std::shared_ptr<Peer> p, Peer::Buffer&& msg) {
        uint32_t hbh = extractHopByHop(msg);
        p->send(buildRaa(hbh));
    });

    Peer::Buffer answer;
    std::atomic<bool> gotAnswer{false};
    server.setPeerEventCallback([&](std::shared_ptr<Peer> peer, Peer::State s) {
        if (s == Peer::State::Open) {
            // hbh=0 -> sendRequest must assign a non-zero hop-by-hop.
            server.sendRequest(
                peer, buildRar(0),
                [&](const Peer::Buffer& raa) {
                    answer = raa;
                    gotAnswer = true;
                },
                5000);
        }
    });

    client->connect("127.0.0.1", 13874);
    runFor(std::chrono::milliseconds(700));

    ASSERT_TRUE(gotAnswer);
    ASSERT_GE(answer.size(), 20u);
    EXPECT_EQ(answer[4] & 0x80, 0);  // it is an answer (R-bit clear)
    uint32_t cmdCode = (uint32_t(answer[5]) << 16) | (uint32_t(answer[6]) << 8) | uint32_t(answer[7]);
    EXPECT_EQ(cmdCode, 258u);  // RAA

    server.close();
}

TEST_F(DiameterServer_test, ServerRequestTimesOut) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13875);

    // The connected peer receives the request but never answers.
    auto client = std::make_shared<Peer>(io_, clientConfig());
    client->setRequestCallback([](std::shared_ptr<Peer>, Peer::Buffer&&) {});

    std::atomic<bool> timedOut{false};
    server.setTimeoutCallback([&](uint32_t) { timedOut = true; });
    server.setPeerEventCallback([&](std::shared_ptr<Peer> peer, Peer::State s) {
        if (s == Peer::State::Open) {
            server.sendRequest(
                peer, buildRar(0), [](const Peer::Buffer&) { FAIL() << "Should not receive an answer"; },
                200);  // 200 ms timeout
        }
    });

    client->connect("127.0.0.1", 13875);
    runFor(std::chrono::milliseconds(600));

    EXPECT_TRUE(timedOut);

    server.close();
}

TEST_F(DiameterServer_test, SendRequestFailsWhenPeerNull) {
    DiameterServer server(io_, serverConfig());
    // No peer connected; sending on a null peer must fail cleanly (hbh 0).
    uint32_t hbh = server.sendRequest(nullptr, buildRar(0), [](const Peer::Buffer&) {}, 1000);
    EXPECT_EQ(hbh, 0u);
}

TEST_F(DiameterServer_test, GracefulShutdown) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13871);

    std::atomic<bool> clientClosed{false};

    auto client = std::make_shared<Peer>(io_, clientConfig());
    client->setStateCallback([&](std::shared_ptr<Peer>, Peer::State s) {
        if (s == Peer::State::Closed) clientClosed = true;
    });
    client->connect("127.0.0.1", 13871);

    runFor(std::chrono::milliseconds(300));
    EXPECT_EQ(server.activePeerCount(), 1u);

    server.shutdown(0);
    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(clientClosed);
    EXPECT_EQ(server.activePeerCount(), 0u);
}

TEST_F(DiameterServer_test, StopListeningRejectsNewConnections) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13872);

    // Connect first client
    auto client1 = std::make_shared<Peer>(io_, clientConfig());
    client1->connect("127.0.0.1", 13872);
    runFor(std::chrono::milliseconds(300));
    EXPECT_EQ(server.activePeerCount(), 1u);

    // Stop listening
    server.stopListening();

    // Second client should fail to connect
    std::atomic<bool> client2Closed{false};
    auto client2 = std::make_shared<Peer>(io_, clientConfig());
    client2->setStateCallback([&](std::shared_ptr<Peer>, Peer::State s) {
        if (s == Peer::State::Closed) client2Closed = true;
    });
    client2->connect("127.0.0.1", 13872);
    runFor(std::chrono::milliseconds(500));

    // First client still active
    EXPECT_EQ(server.activePeerCount(), 1u);

    server.close();
}

TEST_F(DiameterServer_test, PeerDisconnectRemovesFromList) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13873);

    auto client = std::make_shared<Peer>(io_, clientConfig());
    client->connect("127.0.0.1", 13873);
    runFor(std::chrono::milliseconds(300));
    EXPECT_EQ(server.activePeerCount(), 1u);

    // Client disconnects
    client->disconnect(0);
    runFor(std::chrono::milliseconds(500));

    EXPECT_EQ(server.activePeerCount(), 0u);
    EXPECT_EQ(server.peers().size(), 0u);

    server.close();
}

// =============================================================================
// SCTP single-homing server (G2). Exercises DiameterServer's SCTP listen path
// plus the Peer SCTP client ctor over a real SCTP association.
// =============================================================================
TEST_F(DiameterServer_test, ListenAndAcceptSinglePeer_SCTP) {
    if (!sctpAvailable()) GTEST_SKIP() << "SCTP not available in this environment";

    DiameterServer server(io_, serverConfig(), Transport::SCTP);
    server.listen("127.0.0.1", 13880);

    std::atomic<bool> peerConnected{false};
    server.setPeerEventCallback([&](std::shared_ptr<Peer>, Peer::State s) {
        if (s == Peer::State::Open) peerConnected = true;
    });

    auto clientPeer = std::make_shared<Peer>(io_, clientConfig(), Transport::SCTP);
    clientPeer->connect("127.0.0.1", 13880);

    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(peerConnected);
    EXPECT_EQ(server.activePeerCount(), 1u);

    clientPeer->close();
    server.close();
}

// =============================================================================
// Answer correlation hardening on the server leg.
// =============================================================================

// Uncorrelated answer: offered to the unmatched callback, never delivered to
// the request callback.
TEST_F(DiameterServer_test, UncorrelatedAnswerOfferedThenDropped) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13890);
    std::atomic<bool> deliveredAsRequest{false};
    std::atomic<int> unmatched{0};
    server.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&&) { deliveredAsRequest = true; });
    server.setUnmatchedAnswerCallback([&](std::shared_ptr<Peer> p, const Peer::Buffer& a) {
        EXPECT_TRUE(p != nullptr);
        EXPECT_EQ(extractHopByHop(a), 0x5555u);
        unmatched++;
        return false;
    });

    auto client = std::make_shared<Peer>(io_, clientConfig());
    client->setStateCallback([&](std::shared_ptr<Peer> p, Peer::State s) {
        if (s == Peer::State::Open) p->send(buildRaa(0x5555));
    });
    client->connect("127.0.0.1", 13890);
    runFor(std::chrono::milliseconds(500));

    EXPECT_EQ(unmatched.load(), 1);
    EXPECT_FALSE(deliveredAsRequest);
    EXPECT_EQ(server.activePeerCount(), 1u);

    server.close();
}

// Server-initiated request answered with a rewritten hop-by-hop: correlated
// by end-to-end; per-transaction timeout not fired.
TEST_F(DiameterServer_test, ServerAnswerCorrelatedByEndToEnd) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13891);

    auto client = std::make_shared<Peer>(io_, clientConfig());
    client->setRequestCallback(
        [&](std::shared_ptr<Peer> p, Peer::Buffer&& msg) { p->send(answerFor(msg, extractHopByHop(msg) + 77)); });

    std::atomic<bool> gotAnswer{false};
    std::atomic<bool> timedOut{false};
    server.setPeerEventCallback([&](std::shared_ptr<Peer> peer, Peer::State s) {
        if (s == Peer::State::Open) {
            server.sendRequest(
                peer, buildRar(0), [&](const Peer::Buffer&) { gotAnswer = true; }, 300, {},
                [&](uint32_t) { timedOut = true; });
        }
    });

    client->connect("127.0.0.1", 13891);
    runFor(std::chrono::milliseconds(700));

    EXPECT_TRUE(gotAnswer);
    EXPECT_FALSE(timedOut);

    server.close();
}

// Per-transaction timeout callback carries the hop-by-hop returned by
// sendRequest, and fires exactly once.
TEST_F(DiameterServer_test, ServerPerTransactionTimeoutCallback) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13892);
    auto client = std::make_shared<Peer>(io_, clientConfig());
    client->setRequestCallback([](std::shared_ptr<Peer>, Peer::Buffer&&) {});

    std::atomic<int> timeouts{0};
    uint32_t sentHbh = 0;
    uint32_t timedOutHbh = 0;
    server.setPeerEventCallback([&](std::shared_ptr<Peer> peer, Peer::State s) {
        if (s == Peer::State::Open) {
            sentHbh = server.sendRequest(
                peer, buildRar(0), [](const Peer::Buffer&) { FAIL() << "no answer expected"; }, 150, {},
                [&](uint32_t hbh) {
                    timedOutHbh = hbh;
                    timeouts++;
                });
        }
    });

    client->connect("127.0.0.1", 13892);
    runFor(std::chrono::milliseconds(600));

    EXPECT_EQ(timeouts.load(), 1);
    EXPECT_NE(sentHbh, 0u);
    EXPECT_EQ(timedOutHbh, sentHbh);

    server.close();
}

// tryResolve() on the server: resolves a pending server-initiated transaction
// with an answer that arrived elsewhere, once; rejects requests.
TEST_F(DiameterServer_test, ServerTryResolve) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 13893);
    Peer::Buffer seen;
    auto client = std::make_shared<Peer>(io_, clientConfig());
    client->setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&& msg) { seen = msg; });

    std::atomic<int> answers{0};
    server.setPeerEventCallback([&](std::shared_ptr<Peer> peer, Peer::State s) {
        if (s == Peer::State::Open) {
            server.sendRequest(peer, buildRar(0), [&](const Peer::Buffer&) { answers++; }, 2000);
        }
    });

    client->connect("127.0.0.1", 13893);
    runFor(std::chrono::milliseconds(400));
    ASSERT_GE(seen.size(), 20u);

    EXPECT_FALSE(server.tryResolve(seen));  // request
    EXPECT_TRUE(server.tryResolve(answerFor(seen)));
    EXPECT_FALSE(server.tryResolve(answerFor(seen)));  // already resolved
    EXPECT_EQ(answers.load(), 1);

    server.close();
}
