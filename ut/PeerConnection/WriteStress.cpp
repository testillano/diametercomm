// Stress tests for PeerConnection::asyncWrite under concurrent writers.
//
// Why: the gateway calls asyncWrite from several threads at once (HTTP/2
// server thread for outbound requests, io_context threads for answers / DWR)
// while the io_context itself runs on N threads. If the peer reads slowly the
// TCP send buffer fills, async_write becomes partial (a chain of write_some)
// and several of them overlap on the same socket: messages interleave on the
// wire and the socket is operated from several threads (undefined behaviour).
//
// The server side reads with a plain blocking socket on its own thread, so the
// framing check does not depend on the code under test. Every message carries
// its own index and a payload pattern derived from it: any interleaving shows
// up as a bad length, a bad pattern or a lost/duplicated index.

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ert/diametercomm/PeerConnection.hpp>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace ert::diametercomm;

namespace {

constexpr std::size_t kHeaderSize = 20;

// Message 'index' with 'payloadSize' bytes: index in hop-by-hop, payload byte
// i = (index + i) & 0xFF, so a fragment of another message breaks the pattern.
PeerConnection::Buffer buildIndexedMessage(uint32_t index, std::size_t payloadSize) {
    const uint32_t totalLen = static_cast<uint32_t>(kHeaderSize + payloadSize);
    PeerConnection::Buffer msg(totalLen, 0);
    msg[0] = 1;
    msg[1] = static_cast<uint8_t>(totalLen >> 16);
    msg[2] = static_cast<uint8_t>(totalLen >> 8);
    msg[3] = static_cast<uint8_t>(totalLen);
    msg[4] = 0x80;  // request
    msg[7] = 0x10;  // command 272 (CCR) low byte: 0x000110
    msg[6] = 0x01;
    msg[12] = static_cast<uint8_t>(index >> 24);
    msg[13] = static_cast<uint8_t>(index >> 16);
    msg[14] = static_cast<uint8_t>(index >> 8);
    msg[15] = static_cast<uint8_t>(index);
    for (std::size_t i = 0; i < payloadSize; ++i) msg[kHeaderSize + i] = static_cast<uint8_t>((index + i) & 0xFF);
    return msg;
}

bool readExact(int fd, uint8_t* buf, std::size_t len) {
    std::size_t done = 0;
    while (done < len) {
        ssize_t n = ::recv(fd, buf + done, len - done, 0);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

struct ReaderResult {
    std::size_t messages{0};
    std::size_t badFrames{0};  // wrong version/length/pattern
    std::set<uint32_t> indexes;
    std::size_t duplicates{0};
    std::string firstError;
};

// Blocking reader on its own thread: small receive buffer and a short pause
// every few messages, so the client send buffer fills and writes go partial.
// Stops after 'expected' messages, on EOF, or on a framing error (after a
// framing error the stream position is meaningless).
ReaderResult slowRead(int fd, std::size_t expected, std::size_t payloadSize) {
    ReaderResult r;
    std::vector<uint8_t> buf(kHeaderSize + payloadSize);
    while (r.messages < expected) {
        if (!readExact(fd, buf.data(), kHeaderSize)) {
            if (r.firstError.empty()) r.firstError = "EOF/error reading header";
            break;
        }
        const uint32_t len = (uint32_t(buf[1]) << 16) | (uint32_t(buf[2]) << 8) | uint32_t(buf[3]);
        const uint32_t index =
            (uint32_t(buf[12]) << 24) | (uint32_t(buf[13]) << 16) | (uint32_t(buf[14]) << 8) | uint32_t(buf[15]);
        if (buf[0] != 1 || len != kHeaderSize + payloadSize) {
            ++r.badFrames;
            r.firstError = "bad header at message " + std::to_string(r.messages) + " (version " +
                           std::to_string(buf[0]) + ", length " + std::to_string(len) + ")";
            break;
        }
        if (!readExact(fd, buf.data() + kHeaderSize, payloadSize)) {
            if (r.firstError.empty()) r.firstError = "EOF/error reading payload";
            break;
        }
        bool patternOk = true;
        for (std::size_t i = 0; i < payloadSize; ++i) {
            if (buf[kHeaderSize + i] != static_cast<uint8_t>((index + i) & 0xFF)) {
                patternOk = false;
                break;
            }
        }
        if (!patternOk) {
            ++r.badFrames;
            r.firstError = "payload of message index " + std::to_string(index) + " mixed with another message";
            break;
        }
        if (!r.indexes.insert(index).second) ++r.duplicates;
        ++r.messages;
        if (r.messages % 64 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return r;
}

}  // namespace

class PeerConnectionWriteStress_test : public ::testing::Test {
   protected:
    // Message shape: tens of KB fill the (shrunk) socket buffers quickly.
    static constexpr std::size_t kPayload = 16 * 1024;
    static constexpr int kIoThreads = 16;

    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;
    std::vector<std::thread> ioThreads_;
    int listenFd_{-1};
    int serverFd_{-1};
    uint16_t port_{0};
    std::shared_ptr<PeerConnection> client_;

    void SetUp() override {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(listenFd_, 0);
        int one = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        int rcvbuf = 4096;  // inherited by the accepted socket
        ::setsockopt(listenFd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        ASSERT_EQ(::listen(listenFd_, 1), 0);
        socklen_t alen = sizeof(addr);
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &alen);
        port_ = ntohs(addr.sin_port);
    }

    void TearDown() override {
        if (client_) client_->close();
        work_.reset();
        io_.stop();
        for (auto& t : ioThreads_) t.join();
        if (serverFd_ >= 0) ::close(serverFd_);
        if (listenFd_ >= 0) ::close(listenFd_);
    }

    // Connect the client (io_context on 'ioThreads' threads) and accept it.
    void connect(int ioThreads) {
        work_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
            boost::asio::make_work_guard(io_));
        for (int i = 0; i < ioThreads; ++i) ioThreads_.emplace_back([this]() { io_.run(); });

        std::atomic<bool> connected{false};
        std::atomic<bool> failed{false};
        client_ = std::make_shared<PeerConnection>(io_);
        client_->asyncConnect(
            "127.0.0.1", port_, [&]() { connected = true; }, [&](const boost::system::error_code&) { failed = true; });
        serverFd_ = ::accept(listenFd_, nullptr, nullptr);
        ASSERT_GE(serverFd_, 0);
        for (int i = 0; i < 200 && !connected && !failed; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ASSERT_TRUE(connected);
        // Shrink the client send buffer too (option applied on the asio socket).
        boost::system::error_code ec;
        client_->socket().set_option(boost::asio::socket_base::send_buffer_size(4096), ec);
    }

    static void expectIntact(const ReaderResult& r, std::size_t expected) {
        EXPECT_EQ(r.badFrames, 0u) << r.firstError;
        EXPECT_EQ(r.duplicates, 0u);
        EXPECT_EQ(r.messages, expected) << r.firstError;
        EXPECT_EQ(r.indexes.size(), expected);
    }
};

// Control: a single writer, next write started only when the previous one
// completes (onComplete chaining). This is the contract asyncWrite needs today;
// must be clean on any io thread count.
TEST_F(PeerConnectionWriteStress_test, ChainedSingleWriterKeepsFraming) {
    constexpr std::size_t kMessages = 2000;
    connect(kIoThreads);

    std::thread reader([&]() {
        auto r = slowRead(serverFd_, kMessages, kPayload);
        expectIntact(r, kMessages);
    });

    // Shared state outlives the test body: the last completion may still be
    // running on an io thread when the reader is done.
    struct Chain {
        std::atomic<uint32_t> next{0};
        std::atomic<std::size_t> completed{0};
        std::function<void()> writeNext;
    };
    auto chain = std::make_shared<Chain>();
    std::weak_ptr<Chain> weak = chain;
    auto conn = client_;
    chain->writeNext = [weak, conn]() {
        auto c = weak.lock();
        if (!c) return;
        uint32_t i = c->next.fetch_add(1);
        if (i >= kMessages) return;
        conn->asyncWrite(buildIndexedMessage(i, kPayload), [weak]() {
            if (auto c2 = weak.lock()) {
                c2->completed.fetch_add(1);
                c2->writeNext();
            }
        });
    };
    boost::asio::post(io_, chain->writeNext);

    reader.join();
    for (int i = 0; i < 300 && chain->completed.load() < kMessages; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(chain->completed.load(), kMessages);
}

// Gateway pattern: several foreign threads (not running the io_context) call
// asyncWrite concurrently while the io_context runs on many threads and the
// peer reads slowly. Expected to FAIL (broken framing) and to be reported by
// TSan until asyncWrite serializes writes (strand + write queue).
TEST_F(PeerConnectionWriteStress_test, ConcurrentForeignWritersKeepFraming) {
    constexpr int kWriters = 4;
    constexpr std::size_t kPerWriter = 500;
    constexpr std::size_t kMessages = kWriters * kPerWriter;
    connect(kIoThreads);

    ReaderResult result;
    std::atomic<bool> readerDone{false};
    std::thread reader([&]() {
        result = slowRead(serverFd_, kMessages, kPayload);
        readerDone = true;
    });

    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w]() {
            for (std::size_t i = 0; i < kPerWriter; ++i) {
                client_->asyncWrite(buildIndexedMessage(static_cast<uint32_t>(w * kPerWriter + i), kPayload));
            }
        });
    }
    for (auto& t : writers) t.join();

    // On broken framing the reader stops early: unblock it if it waits forever
    // for bytes that will never be a valid message.
    std::thread watchdog([&]() {
        for (int i = 0; i < 3000 && !readerDone; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!readerDone) ::shutdown(serverFd_, SHUT_RDWR);
    });
    reader.join();
    watchdog.join();

    expectIntact(result, kMessages);
}

// TLS variant: ssl::stream keeps per-connection engine state (OpenSSL SSL
// object, output buffer) touched by every write, so overlapping async_write
// is a memory race there, not only an ordering problem. The server is a
// PeerConnection on its own single-threaded io_context whose message handler
// sleeps now and then (slow reader); its own framing check reports corruption
// as a read error (invalid length) or as a payload pattern mismatch.
TEST(PeerConnectionWriteStressTls_test, ConcurrentForeignWritersKeepFraming) {
    char dirTemplate[] = "/tmp/dcomm-tls-XXXXXX";
    const char* dir = ::mkdtemp(dirTemplate);
    ASSERT_NE(dir, nullptr);
    const std::string crt = std::string(dir) + "/s.crt", key = std::string(dir) + "/s.key";
    const std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -subj /CN=localhost -days 1 -keyout " + key +
                            " -out " + crt + " >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) GTEST_SKIP() << "openssl CLI not available";

    constexpr std::size_t kPayload = 16 * 1024;
    constexpr int kWriters = 4;
    constexpr std::size_t kPerWriter = 300;
    constexpr std::size_t kMessages = kWriters * kPerWriter;

    // Server: single thread, slow handler.
    boost::asio::io_context serverIo;
    boost::asio::ip::tcp::acceptor acceptor(serverIo, {boost::asio::ip::make_address("127.0.0.1"), 0});
    acceptor.set_option(boost::asio::socket_base::receive_buffer_size(4096));
    const uint16_t port = acceptor.local_endpoint().port();
    TlsConfig serverTls;
    serverTls.enabled = true;
    serverTls.certFile = crt;
    serverTls.keyFile = key;
    auto serverCtx = PeerConnection::makeServerContext(serverTls);

    std::atomic<std::size_t> received{0}, badPayload{0};
    std::atomic<bool> readError{false};
    std::shared_ptr<PeerConnection> server;
    auto sock = std::make_shared<boost::asio::ip::tcp::socket>(serverIo);
    acceptor.async_accept(*sock, [&](const boost::system::error_code& ec) {
        if (ec) return;
        server = std::make_shared<PeerConnection>(std::move(*sock));
        server->enableTls(serverCtx, /*server*/ true);
        server->asyncHandshakeServer(
            [&]() {
                server->startReading(
                    [&](PeerConnection::Buffer&& msg) {
                        const uint32_t index = (uint32_t(msg[12]) << 24) | (uint32_t(msg[13]) << 16) |
                                               (uint32_t(msg[14]) << 8) | uint32_t(msg[15]);
                        bool ok = msg.size() == kHeaderSize + kPayload;
                        for (std::size_t i = 0; ok && i < kPayload; ++i)
                            ok = msg[kHeaderSize + i] == static_cast<uint8_t>((index + i) & 0xFF);
                        if (!ok) ++badPayload;
                        if (++received % 32 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    },
                    [&](const boost::system::error_code&) { readError = true; });
            },
            [&](const boost::system::error_code&) { readError = true; });
    });
    auto serverWork = boost::asio::make_work_guard(serverIo);
    std::thread serverThread([&]() { serverIo.run(); });

    // Client: io_context on 16 threads, foreign writers.
    boost::asio::io_context io;
    auto work = boost::asio::make_work_guard(io);
    std::vector<std::thread> ioThreads;
    for (int i = 0; i < 16; ++i) ioThreads.emplace_back([&]() { io.run(); });
    TlsConfig clientTls;
    clientTls.enabled = true;
    auto client = std::make_shared<PeerConnection>(io);
    client->enableTls(PeerConnection::makeClientContext(clientTls), /*server*/ false);
    std::atomic<bool> connected{false};
    client->asyncConnect(
        "127.0.0.1", port, [&]() { connected = true; }, [&](const boost::system::error_code&) { readError = true; });
    for (int i = 0; i < 400 && !connected && !readError; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(connected);

    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w]() {
            for (std::size_t i = 0; i < kPerWriter; ++i)
                client->asyncWrite(buildIndexedMessage(static_cast<uint32_t>(w * kPerWriter + i), kPayload));
        });
    }
    for (auto& t : writers) t.join();
    for (int i = 0; i < 1000 && received < kMessages && !readError; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_FALSE(readError) << "server read error (broken TLS record / Diameter framing) after " << received.load();
    EXPECT_EQ(badPayload.load(), 0u);
    EXPECT_EQ(received.load(), kMessages);

    client->close();
    work.reset();
    io.stop();
    for (auto& t : ioThreads) t.join();
    boost::asio::post(serverIo, [&]() {
        if (server) server->close();
        acceptor.close();
    });
    serverWork.reset();
    serverIo.stop();
    serverThread.join();
    ::unlink(crt.c_str());
    ::unlink(key.c_str());
    ::rmdir(dir);
}
