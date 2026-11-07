// Close/reconnect under load (proposal H0b).
//
// Why: in the gateway, DiameterClient::send()/isReady() run on the HTTP/2
// server thread while the io_context runs on N threads. When the connection
// drops, Peer::onError -> onPeerState(Closed) -> scheduleReconnect() and later
// connect() replaces 'peer_' on an io thread, concurrently with those foreign
// readers. This test drives that interleaving: foreign senders, a server that
// keeps resetting the connection, short reconnect backoff. Run under TSan to
// see the unsynchronized accesses (peer_, Peer::state_, reconnectTimer_).
//
// The server runs on its own single-threaded io_context so that only the
// client side is under test.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <ert/diametercomm/DiameterClient.hpp>
#include <ert/diametercomm/DiameterServer.hpp>
#include <iostream>
#include <thread>
#include <vector>

using namespace ert::diametercomm;

namespace {

Peer::Buffer buildCcr() {
    Peer::Buffer msg(20, 0);
    msg[0] = 1;
    msg[3] = 20;
    msg[4] = 0x80;  // R-bit
    msg[6] = 1;
    msg[7] = 0x10;  // 272
    msg[11] = 4;
    return msg;
}

}  // namespace

TEST(DiameterClientCloseUnderLoad_test, ForeignSendersSurviveRepeatedResets) {
    constexpr uint16_t kPort = 14899;
    constexpr int kIoThreads = 16;
    constexpr int kSenders = 4;
    constexpr int kResets = 10;

    // Server: own io_context, one thread; answers every request.
    boost::asio::io_context serverIo;
    auto serverWork = boost::asio::make_work_guard(serverIo);
    DiameterServer server(serverIo, {"server.example.com", "example.com", "127.0.0.1", 0, "TestServer", 0, {4}});
    server.setRequestCallback([&](std::shared_ptr<Peer> peer, Peer::Buffer&& req) {
        Peer::Buffer ans(req.begin(), req.begin() + 20);
        ans[4] = 0x00;
        peer->send(std::move(ans));
    });
    server.listen("127.0.0.1", kPort);
    std::thread serverThread([&]() { serverIo.run(); });

    // Client: io_context on many threads, as the gateway (--workers nproc).
    boost::asio::io_context io;
    auto work = boost::asio::make_work_guard(io);
    auto client = std::make_unique<DiameterClient>(
        io, Peer::Config{"client.example.com", "example.com", "127.0.0.1", 0, "TestClient", 0, {4}});
    client->setReconnectEnabled(true);
    client->setReconnectBackoff(std::chrono::milliseconds(10), std::chrono::milliseconds(20));
    std::atomic<int> opens{0};
    client->setStateCallback([&](Peer::State s) {
        if (s == Peer::State::Open) ++opens;
    });
    std::atomic<long> answered{0}, timedOut{0};
    std::vector<std::thread> ioThreads;
    for (int i = 0; i < kIoThreads; ++i) ioThreads.emplace_back([&]() { io.run(); });
    boost::asio::post(io, [&]() { client->connect("127.0.0.1", kPort); });
    for (int i = 0; i < 200 && opens == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_GT(opens.load(), 0);

    // Foreign senders: never on an io thread (HTTP/2 server thread pattern).
    std::atomic<bool> stop{false};
    std::atomic<long> sent{0}, unsent{0};
    std::vector<std::thread> senders;
    for (int s = 0; s < kSenders; ++s) {
        senders.emplace_back([&]() {
            while (!stop) {
                if (!client->isReady()) {
                    std::this_thread::yield();
                    continue;
                }
                uint32_t hbh = client->send(
                    buildCcr(), [&](const Peer::Buffer&) { ++answered; }, 50, {}, [&](uint32_t) { ++timedOut; });
                (hbh ? sent : unsent)++;
            }
        });
    }

    // Reset the connection from the server side repeatedly, under load.
    for (int r = 0; r < kResets; ++r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        boost::asio::post(serverIo, [&]() {
            for (auto& p : server.peers()) p->close();
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop = true;
    for (auto& t : senders) t.join();

    // The client must have reconnected after the resets and kept working.
    EXPECT_GT(opens.load(), 1);
    EXPECT_GT(sent.load(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let timeouts expire

    // Tear down: close on an io thread, then stop the io before destroying.
    std::atomic<bool> closed{false};
    boost::asio::post(io, [&]() {
        client->setReconnectEnabled(false);
        client->close();
        closed = true;
    });
    for (int i = 0; i < 200 && !closed; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    work.reset();
    io.stop();
    for (auto& t : ioThreads) t.join();
    client.reset();

    boost::asio::post(serverIo, [&]() { server.close(); });
    serverWork.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    serverIo.stop();
    serverThread.join();

    RecordProperty("sent", std::to_string(sent.load()));
    RecordProperty("answered", std::to_string(answered.load()));
    RecordProperty("timedOut", std::to_string(timedOut.load()));
    std::cout << "opens=" << opens << " sent=" << sent << " unsent=" << unsent << " answered=" << answered
              << " timedOut=" << timedOut << std::endl;
}
