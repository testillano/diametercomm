/*
 _____________________________________________________________________________
|                                                                             |
|      _ _                      _                                             |
|   __| (_) __ _ _ __ ___   ___| |_ ___ _ __ ___ ___  _ __ ___  _ __ ___     |
|  / _` | |/ _` | '_ ` _ \ / _ \ __/ _ \ '__/ __/ _ \| '_ ` _ \| '_ ` _ \   |
| | (_| | | (_| | | | | | |  __/ ||  __/ | | (_| (_) | | | | | | | | | | |   |
|  \__,_|_|\__,_|_| |_| |_|\___|\__\___|_|  \___\___/|_| |_| |_|_| |_| |_|  |
|                                                                             |
|_____________________________________________________________________________|

C++ DIAMETER COMMUNICATIONS LIBRARY
https://github.com/testillano/diametercomm

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2024 Eduardo Ramos
*/

#include <ert/diametercomm/Peer.hpp>

#include <cstring>
#include <arpa/inet.h>
#include <chrono>

namespace ert
{
namespace diametercomm
{

// Diameter base protocol command codes
static constexpr uint32_t CMD_CER = 257;
static constexpr uint32_t CMD_DWR = 280;
static constexpr uint32_t CMD_DPR = 282;

// Diameter message flags
static constexpr uint8_t FLAG_REQUEST = 0x80;

// Diameter AVP codes (base protocol)
static constexpr uint32_t AVP_ORIGIN_HOST      = 264;
static constexpr uint32_t AVP_ORIGIN_REALM     = 296;
static constexpr uint32_t AVP_HOST_IP_ADDRESS  = 257;
static constexpr uint32_t AVP_VENDOR_ID        = 266;
static constexpr uint32_t AVP_PRODUCT_NAME     = 269;
static constexpr uint32_t AVP_RESULT_CODE      = 268;
static constexpr uint32_t AVP_DISCONNECT_CAUSE = 273;
static constexpr uint32_t AVP_AUTH_APP_ID      = 258;

// Result codes
static constexpr uint32_t DIAMETER_SUCCESS = 2001;

// AVP flags
static constexpr uint8_t AVP_FLAG_MANDATORY = 0x40;

// ============================================================================
// Helper: encode an AVP into a buffer
// ============================================================================
using Buffer = Peer::Buffer;

namespace {

void appendAvp(Buffer& buf, uint32_t code, uint8_t flags,
               const uint8_t* data, uint32_t dataLen) {
    uint32_t avpLen = 8 + dataLen; // header(8) + data
    // Code
    buf.push_back(static_cast<uint8_t>(code >> 24));
    buf.push_back(static_cast<uint8_t>(code >> 16));
    buf.push_back(static_cast<uint8_t>(code >> 8));
    buf.push_back(static_cast<uint8_t>(code));
    // Flags
    buf.push_back(flags);
    // Length (3 bytes)
    buf.push_back(static_cast<uint8_t>(avpLen >> 16));
    buf.push_back(static_cast<uint8_t>(avpLen >> 8));
    buf.push_back(static_cast<uint8_t>(avpLen));
    // Data
    buf.insert(buf.end(), data, data + dataLen);
    // Padding
    uint32_t pad = (4 - (avpLen % 4)) % 4;
    for (uint32_t i = 0; i < pad; ++i) buf.push_back(0);
}

void appendStringAvp(Buffer& buf, uint32_t code, uint8_t flags,
                     const std::string& value) {
    appendAvp(buf, code, flags,
              reinterpret_cast<const uint8_t*>(value.data()),
              static_cast<uint32_t>(value.size()));
}

void appendUint32Avp(Buffer& buf, uint32_t code, uint8_t flags, uint32_t value) {
    uint8_t data[4];
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
    appendAvp(buf, code, flags, data, 4);
}

void appendAddressAvp(Buffer& buf, uint32_t code, uint8_t flags,
                      const std::string& addr) {
    uint8_t ipv4[4];
    uint8_t data[6]; // family(2) + ipv4(4)
    if (inet_pton(AF_INET, addr.c_str(), ipv4) == 1) {
        data[0] = 0; data[1] = 1; // IPv4 family
        std::memcpy(data + 2, ipv4, 4);
        appendAvp(buf, code, flags, data, 6);
    } else {
        // Fallback: encode 0.0.0.0
        data[0] = 0; data[1] = 1;
        data[2] = 0; data[3] = 0; data[4] = 0; data[5] = 0;
        appendAvp(buf, code, flags, data, 6);
    }
}

// Write 20-byte message header
void writeMessageHeader(Buffer& buf, uint8_t flags, uint32_t code,
                        uint32_t appId, uint32_t hbh, uint32_t e2e) {
    buf.push_back(1); // version
    // length placeholder (filled later)
    buf.push_back(0); buf.push_back(0); buf.push_back(0);
    buf.push_back(flags);
    // command code (3 bytes)
    buf.push_back(static_cast<uint8_t>(code >> 16));
    buf.push_back(static_cast<uint8_t>(code >> 8));
    buf.push_back(static_cast<uint8_t>(code));
    // application-id
    buf.push_back(static_cast<uint8_t>(appId >> 24));
    buf.push_back(static_cast<uint8_t>(appId >> 16));
    buf.push_back(static_cast<uint8_t>(appId >> 8));
    buf.push_back(static_cast<uint8_t>(appId));
    // hop-by-hop
    buf.push_back(static_cast<uint8_t>(hbh >> 24));
    buf.push_back(static_cast<uint8_t>(hbh >> 16));
    buf.push_back(static_cast<uint8_t>(hbh >> 8));
    buf.push_back(static_cast<uint8_t>(hbh));
    // end-to-end
    buf.push_back(static_cast<uint8_t>(e2e >> 24));
    buf.push_back(static_cast<uint8_t>(e2e >> 16));
    buf.push_back(static_cast<uint8_t>(e2e >> 8));
    buf.push_back(static_cast<uint8_t>(e2e));
}

// Update message length in the first 4 bytes
void fixMessageLength(Buffer& buf) {
    uint32_t len = static_cast<uint32_t>(buf.size());
    buf[1] = static_cast<uint8_t>(len >> 16);
    buf[2] = static_cast<uint8_t>(len >> 8);
    buf[3] = static_cast<uint8_t>(len);
}

// Extract a string AVP value by code from raw message buffer
std::string extractStringAvp(const Buffer& msg, uint32_t avpCode) {
    size_t pos = 20; // skip message header
    while (pos + 8 <= msg.size()) {
        uint32_t code = (uint32_t(msg[pos]) << 24) | (uint32_t(msg[pos+1]) << 16) |
                        (uint32_t(msg[pos+2]) << 8) | uint32_t(msg[pos+3]);
        uint32_t avpLen = (uint32_t(msg[pos+5]) << 16) |
                          (uint32_t(msg[pos+6]) << 8) | uint32_t(msg[pos+7]);
        uint32_t headerLen = (msg[pos+4] & 0x80) ? 12 : 8; // vendor bit?

        if (avpLen < headerLen || pos + avpLen > msg.size()) break;

        if (code == avpCode) {
            uint32_t dataLen = avpLen - headerLen;
            return std::string(reinterpret_cast<const char*>(msg.data() + pos + headerLen),
                               dataLen);
        }

        pos += ((avpLen + 3) / 4) * 4; // padded
    }
    return "";
}

} // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

Peer::Peer(boost::asio::io_context& io, const Config& config)
    : io_(io)
    , connection_(std::make_shared<PeerConnection>(io))
    , config_(config)
    , watchdogTimer_(io)
{
}

Peer::Peer(std::shared_ptr<PeerConnection> connection,
           boost::asio::io_context& io, const Config& config)
    : io_(io)
    , connection_(std::move(connection))
    , config_(config)
    , watchdogTimer_(io)
{
}

Peer::~Peer() {
    stopWatchdog();
}

// ============================================================================
// connect (client-side)
// ============================================================================
void Peer::connect(const std::string& host, uint16_t port) {
    auto self = shared_from_this();
    connection_->asyncConnect(host, port,
        [self]() {
            // Connected: send CER
            self->setState(State::WaitCEA);
            auto cer = self->buildCER();
            self->connection_->asyncWrite(std::move(cer));
            // Start reading
            self->connection_->startReading(
                [self](Buffer&& msg) { self->onMessage(std::move(msg)); },
                [self](const boost::system::error_code& ec) { self->onError(ec); });
        },
        [self](const boost::system::error_code& ec) {
            self->onError(ec);
        });
}

// ============================================================================
// start (server-side)
// ============================================================================
void Peer::start() {
    setState(State::WaitCER);
    auto self = shared_from_this();
    connection_->startReading(
        [self](Buffer&& msg) { self->onMessage(std::move(msg)); },
        [self](const boost::system::error_code& ec) { self->onError(ec); });
}

// ============================================================================
// send (application message)
// ============================================================================
bool Peer::send(Buffer msg) {
    if (state_ != State::Open) return false;
    if (msg.size() < 20) return false;

    // Set hop-by-hop if zero
    uint32_t hbh = extractHopByHop(msg);
    if (hbh == 0) {
        setHopByHop(msg, nextHopByHop());
    }

    // Set end-to-end if zero
    uint32_t e2e = extractEndToEnd(msg);
    if (e2e == 0) {
        setEndToEnd(msg, nextEndToEnd());
    }

    connection_->asyncWrite(std::move(msg));
    return true;
}

// ============================================================================
// disconnect (graceful DPR/DPA)
// ============================================================================
void Peer::disconnect(uint32_t disconnectCause) {
    if (state_ != State::Open) {
        close();
        return;
    }

    setState(State::Closing);
    stopWatchdog();

    Buffer dpr;
    writeMessageHeader(dpr, FLAG_REQUEST, CMD_DPR, 0,
                       nextHopByHop(), nextEndToEnd());
    appendStringAvp(dpr, AVP_ORIGIN_HOST, AVP_FLAG_MANDATORY, config_.originHost);
    appendStringAvp(dpr, AVP_ORIGIN_REALM, AVP_FLAG_MANDATORY, config_.originRealm);
    appendUint32Avp(dpr, AVP_DISCONNECT_CAUSE, AVP_FLAG_MANDATORY, disconnectCause);
    fixMessageLength(dpr);

    connection_->asyncWrite(std::move(dpr));
}

// ============================================================================
// close (force)
// ============================================================================
void Peer::close() {
    stopWatchdog();
    if (connection_) connection_->close();
    setState(State::Closed);
}

// ============================================================================
// onMessage - route incoming messages to appropriate handler
// ============================================================================
void Peer::onMessage(Buffer&& msg) {
    if (msg.size() < 20) return; // invalid

    uint32_t cmdCode = extractCommandCode(msg);
    bool request = isRequest(msg);

    // Base protocol handling
    if (cmdCode == CMD_CER && request) {
        handleCER(msg);
    } else if (cmdCode == CMD_CER && !request) {
        handleCEA(msg);
    } else if (cmdCode == CMD_DWR && request) {
        handleDWR(msg);
    } else if (cmdCode == CMD_DWR && !request) {
        handleDWA(msg);
    } else if (cmdCode == CMD_DPR && request) {
        handleDPR(msg);
    } else if (cmdCode == CMD_DPR && !request) {
        handleDPA(msg);
    } else {
        // Application-level message
        if (state_ == State::Open && onRequest_) {
            onRequest_(shared_from_this(), std::move(msg));
        }
    }
}

// ============================================================================
// onError
// ============================================================================
void Peer::onError(const boost::system::error_code& /*ec*/) {
    stopWatchdog();
    setState(State::Closed);
}

// ============================================================================
// handleCER (server receives CER, sends CEA)
// ============================================================================
void Peer::handleCER(const Buffer& msg) {
    if (state_ != State::WaitCER) return;

    remoteOriginHost_ = extractStringAvp(msg, AVP_ORIGIN_HOST);
    remoteOriginRealm_ = extractStringAvp(msg, AVP_ORIGIN_REALM);

    auto cea = buildCEA(msg);
    connection_->asyncWrite(std::move(cea));

    setState(State::Open);
    startWatchdog();
}

// ============================================================================
// handleCEA (client receives CEA)
// ============================================================================
void Peer::handleCEA(const Buffer& msg) {
    if (state_ != State::WaitCEA) return;

    remoteOriginHost_ = extractStringAvp(msg, AVP_ORIGIN_HOST);
    remoteOriginRealm_ = extractStringAvp(msg, AVP_ORIGIN_REALM);

    setState(State::Open);
    startWatchdog();
}

// ============================================================================
// handleDWR (receive DWR, auto-respond DWA)
// ============================================================================
void Peer::handleDWR(const Buffer& msg) {
    if (state_ != State::Open) return;
    auto dwa = buildDWA(msg);
    connection_->asyncWrite(std::move(dwa));
}

// ============================================================================
// handleDWA (receive DWA - watchdog response, just acknowledge)
// ============================================================================
void Peer::handleDWA(const Buffer& /*msg*/) {
    // Watchdog answered - peer is alive. Nothing else to do.
}

// ============================================================================
// handleDPR (receive DPR, send DPA, close)
// ============================================================================
void Peer::handleDPR(const Buffer& msg) {
    auto dpa = buildDPA(msg);
    connection_->asyncWrite(std::move(dpa), [this]() {
        close();
    });
}

// ============================================================================
// handleDPA (receive DPA after we sent DPR)
// ============================================================================
void Peer::handleDPA(const Buffer& /*msg*/) {
    if (state_ != State::Closing) return;
    close();
}

// ============================================================================
// buildCER
// ============================================================================
Peer::Buffer Peer::buildCER() const {
    Buffer msg;
    writeMessageHeader(msg, FLAG_REQUEST, CMD_CER, 0,
                       const_cast<Peer*>(this)->nextHopByHop(),
                       const_cast<Peer*>(this)->nextEndToEnd());

    appendStringAvp(msg, AVP_ORIGIN_HOST, AVP_FLAG_MANDATORY, config_.originHost);
    appendStringAvp(msg, AVP_ORIGIN_REALM, AVP_FLAG_MANDATORY, config_.originRealm);
    appendAddressAvp(msg, AVP_HOST_IP_ADDRESS, AVP_FLAG_MANDATORY, config_.hostIpAddress);
    appendUint32Avp(msg, AVP_VENDOR_ID, AVP_FLAG_MANDATORY, config_.vendorId);
    appendStringAvp(msg, AVP_PRODUCT_NAME, 0, config_.productName);
    if (config_.applicationId != 0) {
        appendUint32Avp(msg, AVP_AUTH_APP_ID, AVP_FLAG_MANDATORY, config_.applicationId);
    }
    fixMessageLength(msg);
    return msg;
}

// ============================================================================
// buildCEA
// ============================================================================
Peer::Buffer Peer::buildCEA(const Buffer& cer) const {
    Buffer msg;
    uint32_t hbh = extractHopByHop(cer);
    uint32_t e2e = extractEndToEnd(cer);

    writeMessageHeader(msg, 0 /*answer: no R-flag*/, CMD_CER, 0, hbh, e2e);

    appendUint32Avp(msg, AVP_RESULT_CODE, AVP_FLAG_MANDATORY, DIAMETER_SUCCESS);
    appendStringAvp(msg, AVP_ORIGIN_HOST, AVP_FLAG_MANDATORY, config_.originHost);
    appendStringAvp(msg, AVP_ORIGIN_REALM, AVP_FLAG_MANDATORY, config_.originRealm);
    appendAddressAvp(msg, AVP_HOST_IP_ADDRESS, AVP_FLAG_MANDATORY, config_.hostIpAddress);
    appendUint32Avp(msg, AVP_VENDOR_ID, AVP_FLAG_MANDATORY, config_.vendorId);
    appendStringAvp(msg, AVP_PRODUCT_NAME, 0, config_.productName);
    if (config_.applicationId != 0) {
        appendUint32Avp(msg, AVP_AUTH_APP_ID, AVP_FLAG_MANDATORY, config_.applicationId);
    }
    fixMessageLength(msg);
    return msg;
}

// ============================================================================
// buildDWR
// ============================================================================
Peer::Buffer Peer::buildDWR() {
    Buffer msg;
    writeMessageHeader(msg, FLAG_REQUEST, CMD_DWR, 0,
                       nextHopByHop(), nextEndToEnd());
    appendStringAvp(msg, AVP_ORIGIN_HOST, AVP_FLAG_MANDATORY, config_.originHost);
    appendStringAvp(msg, AVP_ORIGIN_REALM, AVP_FLAG_MANDATORY, config_.originRealm);
    fixMessageLength(msg);
    return msg;
}

// ============================================================================
// buildDWA
// ============================================================================
Peer::Buffer Peer::buildDWA(const Buffer& dwr) const {
    Buffer msg;
    uint32_t hbh = extractHopByHop(dwr);
    uint32_t e2e = extractEndToEnd(dwr);

    writeMessageHeader(msg, 0 /*answer*/, CMD_DWR, 0, hbh, e2e);
    appendUint32Avp(msg, AVP_RESULT_CODE, AVP_FLAG_MANDATORY, DIAMETER_SUCCESS);
    appendStringAvp(msg, AVP_ORIGIN_HOST, AVP_FLAG_MANDATORY, config_.originHost);
    appendStringAvp(msg, AVP_ORIGIN_REALM, AVP_FLAG_MANDATORY, config_.originRealm);
    fixMessageLength(msg);
    return msg;
}

// ============================================================================
// buildDPA
// ============================================================================
Peer::Buffer Peer::buildDPA(const Buffer& dpr) const {
    Buffer msg;
    uint32_t hbh = extractHopByHop(dpr);
    uint32_t e2e = extractEndToEnd(dpr);

    writeMessageHeader(msg, 0 /*answer*/, CMD_DPR, 0, hbh, e2e);
    appendUint32Avp(msg, AVP_RESULT_CODE, AVP_FLAG_MANDATORY, DIAMETER_SUCCESS);
    appendStringAvp(msg, AVP_ORIGIN_HOST, AVP_FLAG_MANDATORY, config_.originHost);
    appendStringAvp(msg, AVP_ORIGIN_REALM, AVP_FLAG_MANDATORY, config_.originRealm);
    fixMessageLength(msg);
    return msg;
}

// ============================================================================
// Watchdog
// ============================================================================
void Peer::startWatchdog() {
    if (config_.watchdogIntervalSec == 0) return;

    auto self = shared_from_this();
    watchdogTimer_.expires_after(
        std::chrono::seconds(config_.watchdogIntervalSec));
    watchdogTimer_.async_wait([self](const boost::system::error_code& ec) {
        if (ec) return; // cancelled
        if (self->state_ != State::Open) return;

        auto dwr = self->buildDWR();
        self->connection_->asyncWrite(std::move(dwr));
        self->startWatchdog(); // reschedule
    });
}

void Peer::stopWatchdog() {
    watchdogTimer_.cancel();
}

// ============================================================================
// setState
// ============================================================================
void Peer::setState(State newState) {
    if (state_ == newState) return;
    state_ = newState;
    if (onState_) {
        onState_(shared_from_this(), newState);
    }
}

// ============================================================================
// Static header helpers
// ============================================================================
uint32_t Peer::extractCommandCode(const Buffer& msg) {
    return (uint32_t(msg[5]) << 16) | (uint32_t(msg[6]) << 8) | uint32_t(msg[7]);
}

bool Peer::isRequest(const Buffer& msg) {
    return (msg[4] & FLAG_REQUEST) != 0;
}

uint32_t Peer::extractHopByHop(const Buffer& msg) {
    return (uint32_t(msg[12]) << 24) | (uint32_t(msg[13]) << 16) |
           (uint32_t(msg[14]) << 8) | uint32_t(msg[15]);
}

uint32_t Peer::extractEndToEnd(const Buffer& msg) {
    return (uint32_t(msg[16]) << 24) | (uint32_t(msg[17]) << 16) |
           (uint32_t(msg[18]) << 8) | uint32_t(msg[19]);
}

void Peer::setHopByHop(Buffer& msg, uint32_t hbh) {
    msg[12] = static_cast<uint8_t>(hbh >> 24);
    msg[13] = static_cast<uint8_t>(hbh >> 16);
    msg[14] = static_cast<uint8_t>(hbh >> 8);
    msg[15] = static_cast<uint8_t>(hbh);
}

void Peer::setEndToEnd(Buffer& msg, uint32_t e2e) {
    msg[16] = static_cast<uint8_t>(e2e >> 24);
    msg[17] = static_cast<uint8_t>(e2e >> 16);
    msg[18] = static_cast<uint8_t>(e2e >> 8);
    msg[19] = static_cast<uint8_t>(e2e);
}

} // namespace diametercomm
} // namespace ert
