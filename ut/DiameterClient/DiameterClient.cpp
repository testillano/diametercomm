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
    static uint32_t extractEndToEnd(const Peer::Buffer& msg) {
        return (uint32_t(msg[16]) << 24) | (uint32_t(msg[17]) << 16) | (uint32_t(msg[18]) << 8) | uint32_t(msg[19]);
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

// Bidirectional Diameter (RFC 6733), end-to-end: the server PUSHES a
// server-initiated request (e.g. RAR) via sendRequest() down the connection the
// client opened; the client receives it and replies with sendAnswer() on the
// SAME leg; the server correlates the incoming answer (RAA) by hop-by-hop and
// invokes its response callback. Exercises DiameterClient::sendAnswer AND
// DiameterServer::sendRequest correlation together.
TEST_F(DiameterClient_test, ClientAnswersServerInitiatedRequest) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14874);

    // Client: when it receives the server-initiated request, reply on the same
    // connection with a correlated answer via sendAnswer().
    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);
    std::atomic<bool> clientReceivedRequest{false};
    client.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&& msg) {
        clientReceivedRequest = true;
        uint32_t hbh = extractHopByHop(msg);
        EXPECT_TRUE((msg[4] & 0x80) != 0);  // it is a request (R-bit set)
        client.sendAnswer(buildAppAnswer(hbh));
    });

    // Server: on peer Open, push the request via sendRequest and capture the
    // correlated answer through the response callback.
    Peer::Buffer answerAtServer;
    std::atomic<bool> serverGotAnswer{false};
    server.setPeerEventCallback([&](std::shared_ptr<Peer> peer, Peer::State s) {
        if (s == Peer::State::Open) {
            server.sendRequest(
                peer, buildAppRequest(),  // hbh=0 -> server assigns it
                [&](const Peer::Buffer& raa) {
                    answerAtServer = raa;
                    serverGotAnswer = true;
                },
                5000);
        }
    });

    client.connect("127.0.0.1", 14874);
    runFor(std::chrono::milliseconds(700));

    ASSERT_TRUE(clientReceivedRequest);
    ASSERT_TRUE(serverGotAnswer);
    ASSERT_GE(answerAtServer.size(), 20u);
    EXPECT_EQ(answerAtServer[4] & 0x80, 0);  // it is an answer (R-bit clear)

    client.close();
    server.close();
}

// sendAnswer() must fail cleanly when the client is not connected (no peer),
// mirroring send()'s not-connected guard.
TEST_F(DiameterClient_test, SendAnswerFailsWhenNotConnected) {
    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);
    EXPECT_FALSE(client.sendAnswer(buildAppAnswer(0x1)));
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

// =============================================================================
// Answer correlation hardening: end-to-end fallback, cross-leg resolution,
// uncorrelated answers never delivered as requests, per-transaction timeout.
// =============================================================================

// A relay/stack that re-originates the hop-by-hop still keeps the end-to-end
// (RFC 6733): the answer must be correlated by end-to-end.
TEST_F(DiameterClient_test, AnswerCorrelatedByEndToEndWhenHopByHopRewritten) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14890);
    uint32_t sentE2e = 0;
    server.setRequestCallback([&](std::shared_ptr<Peer> peer, Peer::Buffer&& msg) {
        sentE2e = extractEndToEnd(msg);
        peer->send(answerFor(msg, extractHopByHop(msg) + 1000));
    });

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);
    std::atomic<bool> gotResponse{false};
    std::atomic<bool> unmatched{false};
    std::atomic<bool> deliveredAsRequest{false};
    uint32_t answerE2e = 0;
    client.setUnmatchedAnswerCallback([&](const Peer::Buffer&) {
        unmatched = true;
        return false;
    });
    client.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&&) { deliveredAsRequest = true; });
    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) {
            client.send(
                buildAppRequest(),
                [&](const Peer::Buffer& resp) {
                    answerE2e = extractEndToEnd(resp);
                    gotResponse = true;
                },
                2000);
        }
    });

    client.connect("127.0.0.1", 14890);
    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(gotResponse);
    EXPECT_NE(sentE2e, 0u);  // client assigns e2e itself (fallback key)
    EXPECT_EQ(answerE2e, sentE2e);
    EXPECT_FALSE(unmatched);
    EXPECT_FALSE(deliveredAsRequest);

    client.close();
    server.close();
}

// An answer matching no pending transaction must never reach the request
// callback (the application would answer an answer). The unmatched-answer
// callback is offered first.
TEST_F(DiameterClient_test, UncorrelatedAnswerIsDroppedNotDeliveredAsRequest) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14891);
    server.setPeerEventCallback([&](std::shared_ptr<Peer> peer, Peer::State s) {
        if (s == Peer::State::Open) peer->send(buildAppAnswer(0x1234));
    });

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);
    std::atomic<bool> deliveredAsRequest{false};
    std::atomic<int> unmatched{0};
    client.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&&) { deliveredAsRequest = true; });
    client.setUnmatchedAnswerCallback([&](const Peer::Buffer& a) {
        EXPECT_EQ(extractHopByHop(a), 0x1234u);
        unmatched++;
        return false;
    });

    client.connect("127.0.0.1", 14891);
    runFor(std::chrono::milliseconds(500));

    EXPECT_EQ(unmatched.load(), 1);
    EXPECT_FALSE(deliveredAsRequest);

    client.close();
    server.close();
}

// Without an unmatched-answer callback the answer is dropped as well.
TEST_F(DiameterClient_test, UncorrelatedAnswerDroppedWithoutUnmatchedCallback) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14892);
    server.setPeerEventCallback([&](std::shared_ptr<Peer> peer, Peer::State s) {
        if (s == Peer::State::Open) peer->send(buildAppAnswer(0x4321));
    });

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);
    std::atomic<bool> deliveredAsRequest{false};
    client.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&&) { deliveredAsRequest = true; });

    client.connect("127.0.0.1", 14892);
    runFor(std::chrono::milliseconds(500));

    EXPECT_FALSE(deliveredAsRequest);
    EXPECT_TRUE(client.isReady());  // connection unaffected

    client.close();
    server.close();
}

// Late answer (after the transaction timeout): the per-transaction timeout
// callback fires once with the hop-by-hop, and the late answer is dropped
// (neither the response callback nor the request callback).
TEST_F(DiameterClient_test, LateAnswerAfterTimeoutIsDropped) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14893);
    auto delay = std::make_shared<boost::asio::steady_timer>(io_);
    server.setRequestCallback([&, delay](std::shared_ptr<Peer> peer, Peer::Buffer&& msg) {
        auto answer = answerFor(msg);
        delay->expires_after(std::chrono::milliseconds(300));
        delay->async_wait([peer, answer](const boost::system::error_code& ec) {
            if (!ec) peer->send(answer);
        });
    });

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);
    std::atomic<bool> deliveredAsRequest{false};
    std::atomic<bool> gotResponse{false};
    std::atomic<int> timeouts{0};
    std::atomic<int> unmatched{0};
    uint32_t sentHbh = 0;
    uint32_t timedOutHbh = 0;
    client.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&&) { deliveredAsRequest = true; });
    client.setUnmatchedAnswerCallback([&](const Peer::Buffer&) {
        unmatched++;
        return false;
    });
    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) {
            sentHbh = client.send(
                buildAppRequest(), [&](const Peer::Buffer&) { gotResponse = true; }, 100, {},
                [&](uint32_t hbh) {
                    timedOutHbh = hbh;
                    timeouts++;
                });
        }
    });

    client.connect("127.0.0.1", 14893);
    runFor(std::chrono::milliseconds(700));

    EXPECT_EQ(timeouts.load(), 1);
    EXPECT_NE(sentHbh, 0u);
    EXPECT_EQ(timedOutHbh, sentHbh);
    EXPECT_FALSE(gotResponse);
    EXPECT_EQ(unmatched.load(), 1);  // the late answer was offered, then dropped
    EXPECT_FALSE(deliveredAsRequest);

    client.close();
    server.close();
}

// Exact NetLoc case: the request goes out on the client leg, but the stack
// returns the answer on ANOTHER connection (the one it opened towards our
// server leg). The server leg misses, its unmatched callback asks the client
// to tryResolve(), and the original response callback fires.
TEST_F(DiameterClient_test, AnswerOnOtherLegResolvedViaTryResolve) {
    // SUT side: a server accepting our client leg, plus a peer that connects to
    // our server leg. The SUT answers on the second connection.
    DiameterServer sut(io_, serverConfig());
    sut.listen("127.0.0.1", 14894);
    DiameterServer gwServer(io_, clientConfig());
    gwServer.listen("127.0.0.1", 14895);
    auto sutToGw = std::make_shared<Peer>(io_, serverConfig());

    sut.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&& msg) { sutToGw->send(answerFor(msg)); });

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);
    std::atomic<bool> gotResponse{false};
    std::atomic<bool> gwServerGotRequest{false};
    gwServer.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&&) { gwServerGotRequest = true; });
    gwServer.setUnmatchedAnswerCallback(
        [&](std::shared_ptr<Peer>, const Peer::Buffer& a) { return client.tryResolve(a); });

    sutToGw->setStateCallback([&](std::shared_ptr<Peer>, Peer::State s) {
        if (s == Peer::State::Open) client.connect("127.0.0.1", 14894);
    });
    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) {
            client.send(buildAppRequest(), [&](const Peer::Buffer&) { gotResponse = true; }, 2000);
        }
    });

    sutToGw->connect("127.0.0.1", 14895);
    runFor(std::chrono::milliseconds(800));

    EXPECT_TRUE(gotResponse);
    EXPECT_FALSE(gwServerGotRequest);

    client.close();
    sutToGw->close();
    gwServer.close();
    sut.close();
}

// tryResolve() only accepts answers whose command code matches the pending
// transaction, and never requests.
TEST_F(DiameterClient_test, TryResolveRejectsRequestsAndCommandMismatch) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 14896);
    Peer::Buffer seenRequest;
    server.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&& msg) { seenRequest = msg; });

    DiameterClient client(io_, clientConfig());
    client.setReconnectEnabled(false);
    std::atomic<int> responses{0};
    client.setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) client.send(buildAppRequest(), [&](const Peer::Buffer&) { responses++; }, 2000);
    });

    client.connect("127.0.0.1", 14896);
    runFor(std::chrono::milliseconds(400));
    ASSERT_GE(seenRequest.size(), 20u);

    // Request (R-bit set) is never resolved.
    EXPECT_FALSE(client.tryResolve(seenRequest));
    // Short buffer.
    EXPECT_FALSE(client.tryResolve(Peer::Buffer(10, 0)));
    // Same hbh/e2e but another command code (258 instead of 272).
    auto wrongCmd = answerFor(seenRequest);
    wrongCmd[7] = 0x02;
    EXPECT_FALSE(client.tryResolve(wrongCmd));
    EXPECT_EQ(responses.load(), 0);
    // Good answer resolves exactly once.
    auto good = answerFor(seenRequest);
    EXPECT_TRUE(client.tryResolve(good));
    EXPECT_FALSE(client.tryResolve(good));
    EXPECT_EQ(responses.load(), 1);

    client.close();
    server.close();
}
