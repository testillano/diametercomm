#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <ert/diametercomm/DiameterServer.hpp>

using namespace ert::diametercomm;

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
