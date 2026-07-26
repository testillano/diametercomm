#include <ert/diametercomm/PeerConnection.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <vector>

using namespace ert::diametercomm;

// Helper: build a minimal valid Diameter message buffer (20 bytes)
static PeerConnection::Buffer buildMinimalMessage(uint32_t hbh = 0x12345678) {
    PeerConnection::Buffer msg(20, 0);
    msg[0] = 1;
    msg[1] = 0; msg[2] = 0; msg[3] = 20;
    msg[4] = 0x80;
    msg[5] = 0; msg[6] = 1; msg[7] = 0x18; // DWR = 280
    msg[12] = static_cast<uint8_t>(hbh >> 24);
    msg[13] = static_cast<uint8_t>(hbh >> 16);
    msg[14] = static_cast<uint8_t>(hbh >> 8);
    msg[15] = static_cast<uint8_t>(hbh);
    msg[16] = 0xDE; msg[17] = 0xAD; msg[18] = 0xBE; msg[19] = 0xEF;
    return msg;
}

static PeerConnection::Buffer buildMessageWithPayload(size_t payloadSize) {
    uint32_t totalLen = 20 + payloadSize;
    PeerConnection::Buffer msg(totalLen, 0);
    msg[0] = 1;
    msg[1] = static_cast<uint8_t>(totalLen >> 16);
    msg[2] = static_cast<uint8_t>(totalLen >> 8);
    msg[3] = static_cast<uint8_t>(totalLen);
    msg[4] = 0x80;
    msg[5] = 0; msg[6] = 1; msg[7] = 0x01;
    for (size_t i = 20; i < totalLen; ++i) msg[i] = static_cast<uint8_t>(i & 0xFF);
    return msg;
}

class PeerConnection_test : public ::testing::Test {
protected:
    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    uint16_t port_{0};

    void SetUp() override {
        acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(
            io_, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::address::from_string("127.0.0.1"), 0));
        port_ = acceptor_->local_endpoint().port();
    }

    void TearDown() override { acceptor_->close(); }

    void runFor(std::chrono::milliseconds timeout) {
        io_.restart();
        io_.run_for(timeout);
    }
};

TEST_F(PeerConnection_test, SendReceiveSingleMessage) {
    auto expectedMsg = buildMinimalMessage();
    PeerConnection::Buffer received;
    std::atomic<bool> messageReceived{false};

    auto serverSock = std::make_shared<boost::asio::ip::tcp::socket>(io_);
    std::shared_ptr<PeerConnection> serverConn;

    acceptor_->async_accept(*serverSock,
        [&](const boost::system::error_code& ec) {
            ASSERT_FALSE(ec) << ec.message();
            serverConn = std::make_shared<PeerConnection>(std::move(*serverSock));
            serverConn->startReading(
                [&](PeerConnection::Buffer&& msg) {
                    received = std::move(msg);
                    messageReceived = true;
                },
                [](const boost::system::error_code&) {});
        });

    auto clientConn = std::make_shared<PeerConnection>(io_);
    clientConn->asyncConnect("127.0.0.1", port_,
        [&]() { clientConn->asyncWrite(expectedMsg); },
        [](const boost::system::error_code& ec) { FAIL() << ec.message(); });

    runFor(std::chrono::milliseconds(500));
    ASSERT_TRUE(messageReceived);
    EXPECT_EQ(received, expectedMsg);
}

TEST_F(PeerConnection_test, SendReceiveMultipleMessages) {
    std::vector<PeerConnection::Buffer> sent;
    std::vector<PeerConnection::Buffer> received;
    for (int i = 0; i < 5; ++i) sent.push_back(buildMinimalMessage(i + 1));

    auto serverSock = std::make_shared<boost::asio::ip::tcp::socket>(io_);
    std::shared_ptr<PeerConnection> serverConn;

    acceptor_->async_accept(*serverSock,
        [&](const boost::system::error_code& ec) {
            ASSERT_FALSE(ec);
            serverConn = std::make_shared<PeerConnection>(std::move(*serverSock));
            serverConn->startReading(
                [&](PeerConnection::Buffer&& msg) { received.push_back(std::move(msg)); },
                [](const boost::system::error_code&) {});
        });

    auto clientConn = std::make_shared<PeerConnection>(io_);
    clientConn->asyncConnect("127.0.0.1", port_,
        [&]() { for (const auto& m : sent) clientConn->asyncWrite(m); },
        [](const boost::system::error_code& ec) { FAIL() << ec.message(); });

    runFor(std::chrono::milliseconds(500));
    ASSERT_EQ(received.size(), 5u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(received[i], sent[i]);
}

TEST_F(PeerConnection_test, SendReceiveLargeMessage) {
    auto largeMsg = buildMessageWithPayload(4000);
    PeerConnection::Buffer received;
    std::atomic<bool> done{false};

    auto serverSock = std::make_shared<boost::asio::ip::tcp::socket>(io_);
    std::shared_ptr<PeerConnection> serverConn;

    acceptor_->async_accept(*serverSock,
        [&](const boost::system::error_code& ec) {
            ASSERT_FALSE(ec);
            serverConn = std::make_shared<PeerConnection>(std::move(*serverSock));
            serverConn->startReading(
                [&](PeerConnection::Buffer&& msg) { received = std::move(msg); done = true; },
                [](const boost::system::error_code&) {});
        });

    auto clientConn = std::make_shared<PeerConnection>(io_);
    clientConn->asyncConnect("127.0.0.1", port_,
        [&]() { clientConn->asyncWrite(largeMsg); },
        [](const boost::system::error_code& ec) { FAIL() << ec.message(); });

    runFor(std::chrono::milliseconds(500));
    ASSERT_TRUE(done);
    EXPECT_EQ(received, largeMsg);
}

TEST_F(PeerConnection_test, ConnectionCloseTriggersError) {
    std::atomic<bool> errorReceived{false};

    auto serverSock = std::make_shared<boost::asio::ip::tcp::socket>(io_);
    std::shared_ptr<PeerConnection> serverConn;

    acceptor_->async_accept(*serverSock,
        [&](const boost::system::error_code& ec) {
            ASSERT_FALSE(ec);
            serverConn = std::make_shared<PeerConnection>(std::move(*serverSock));
            serverConn->startReading(
                [](PeerConnection::Buffer&&) {},
                [&](const boost::system::error_code&) { errorReceived = true; });
        });

    auto clientConn = std::make_shared<PeerConnection>(io_);
    clientConn->asyncConnect("127.0.0.1", port_,
        [&]() { clientConn->close(); },
        [](const boost::system::error_code& ec) { FAIL() << ec.message(); });

    runFor(std::chrono::milliseconds(500));
    EXPECT_TRUE(errorReceived);
}

TEST_F(PeerConnection_test, IsOpenAndClose) {
    std::atomic<bool> connected{false};
    auto clientConn = std::make_shared<PeerConnection>(io_);

    auto serverSock = std::make_shared<boost::asio::ip::tcp::socket>(io_);
    acceptor_->async_accept(*serverSock, [&](const boost::system::error_code&) {});

    clientConn->asyncConnect("127.0.0.1", port_,
        [&]() { connected = true; },
        [](const boost::system::error_code& ec) { FAIL() << ec.message(); });

    runFor(std::chrono::milliseconds(200));
    ASSERT_TRUE(connected);
    EXPECT_TRUE(clientConn->isOpen());
    clientConn->close();
    EXPECT_FALSE(clientConn->isOpen());
}

TEST_F(PeerConnection_test, RemoteEndpoint) {
    std::string endpoint;
    std::atomic<bool> done{false};

    auto serverSock = std::make_shared<boost::asio::ip::tcp::socket>(io_);
    std::shared_ptr<PeerConnection> serverConn;

    acceptor_->async_accept(*serverSock,
        [&](const boost::system::error_code& ec) {
            ASSERT_FALSE(ec);
            serverConn = std::make_shared<PeerConnection>(std::move(*serverSock));
            endpoint = serverConn->remoteEndpoint();
            done = true;
        });

    auto clientConn = std::make_shared<PeerConnection>(io_);
    clientConn->asyncConnect("127.0.0.1", port_, []() {},
        [](const boost::system::error_code& ec) { FAIL() << ec.message(); });

    runFor(std::chrono::milliseconds(200));
    ASSERT_TRUE(done);
    EXPECT_NE(endpoint.find("127.0.0.1:"), std::string::npos);
}

TEST_F(PeerConnection_test, BidirectionalCommunication) {
    auto request = buildMinimalMessage(0x11111111);
    auto response = buildMinimalMessage(0x22222222);
    PeerConnection::Buffer serverReceived, clientReceived;
    std::atomic<bool> done{false};

    auto serverSock = std::make_shared<boost::asio::ip::tcp::socket>(io_);
    std::shared_ptr<PeerConnection> serverConn;

    acceptor_->async_accept(*serverSock,
        [&](const boost::system::error_code& ec) {
            ASSERT_FALSE(ec);
            serverConn = std::make_shared<PeerConnection>(std::move(*serverSock));
            serverConn->startReading(
                [&](PeerConnection::Buffer&& msg) {
                    serverReceived = std::move(msg);
                    serverConn->asyncWrite(response);
                },
                [](const boost::system::error_code&) {});
        });

    auto clientConn = std::make_shared<PeerConnection>(io_);
    clientConn->asyncConnect("127.0.0.1", port_,
        [&]() {
            clientConn->startReading(
                [&](PeerConnection::Buffer&& msg) { clientReceived = std::move(msg); done = true; },
                [](const boost::system::error_code&) {});
            clientConn->asyncWrite(request);
        },
        [](const boost::system::error_code& ec) { FAIL() << ec.message(); });

    runFor(std::chrono::milliseconds(500));
    ASSERT_TRUE(done);
    EXPECT_EQ(serverReceived, request);
    EXPECT_EQ(clientReceived, response);
}

TEST_F(PeerConnection_test, InvalidMessageLengthTriggersError) {
    std::atomic<bool> errorReceived{false};

    auto serverSock = std::make_shared<boost::asio::ip::tcp::socket>(io_);
    std::shared_ptr<PeerConnection> serverConn;

    acceptor_->async_accept(*serverSock,
        [&](const boost::system::error_code& ec) {
            ASSERT_FALSE(ec);
            serverConn = std::make_shared<PeerConnection>(std::move(*serverSock));
            serverConn->startReading(
                [](PeerConnection::Buffer&&) { FAIL() << "Should not receive"; },
                [&](const boost::system::error_code&) { errorReceived = true; });
        });

    auto clientConn = std::make_shared<PeerConnection>(io_);
    clientConn->asyncConnect("127.0.0.1", port_,
        [&]() {
            PeerConnection::Buffer bad = {0x01, 0x00, 0x00, 0x0A}; // length=10 < 20
            boost::asio::async_write(clientConn->socket(),
                boost::asio::buffer(bad),
                [](const boost::system::error_code&, std::size_t) {});
        },
        [](const boost::system::error_code& ec) { FAIL() << ec.message(); });

    runFor(std::chrono::milliseconds(500));
    EXPECT_TRUE(errorReceived);
}
