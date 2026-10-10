#include "network/lan_beacon.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

#include <cstring>

#include "network/ssc_keystream_gen.h"
#include "util/logging.h"

namespace aa3 {
namespace network {

namespace {

constexpr int kMagic = 34983;                // 0x88a7, libaa3 server_lan_thread
constexpr uint16_t kMsgGetServerState = 2;
constexpr uint16_t kCompressFlag = 0x0200;
constexpr const char* kVersion = "3.3";      // what the 3.3 client compares against

// SSC (libaa3 ssc_encrypt/ssc_decrypt): cumulative XOR chain + keystream.
void SscEncrypt(std::vector<uint8_t>& d) {
    for (size_t i = 1; i < d.size(); ++i) d[i] ^= d[i - 1];
    for (size_t i = 0; i < d.size(); ++i) d[i] ^= kAcpuBodyKeystream[i % 8192];
}
void SscDecrypt(std::vector<uint8_t>& d) {
    for (size_t i = 0; i < d.size(); ++i) d[i] ^= kAcpuBodyKeystream[i % 8192];
    uint8_t prev = d.empty() ? 0 : d[0];
    for (size_t i = 1; i < d.size(); ++i) {
        uint8_t cur = d[i];
        d[i] = prev ^ cur;
        prev = cur;
    }
}

void PutU16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xff); v.push_back(x >> 8); }
void PutU32(std::vector<uint8_t>& v, uint32_t x) { for (int k = 0; k < 4; ++k) v.push_back((x >> (8 * k)) & 0xff); }
void PutKey(std::vector<uint8_t>& v, const std::string& k, uint8_t type) {
    PutU16(v, static_cast<uint16_t>(k.size()));
    v.insert(v.end(), k.begin(), k.end());
    v.push_back(type);
}
void PutStr(std::vector<uint8_t>& v, const std::string& s) {  // u16 len incl NUL + bytes + NUL
    PutU16(v, static_cast<uint16_t>(s.size() + 1));
    v.insert(v.end(), s.begin(), s.end());
    v.push_back(0);
}

int OpenUdp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    // REUSEADDR: a client on the same host also binds these LAN ports.
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

}  // namespace

LanBeacon::~LanBeacon() {
    if (query_fd_ >= 0) close(query_fd_);
    if (details_fd_ >= 0) close(details_fd_);
}

bool LanBeacon::Start(uint16_t query_port, uint16_t client_port, uint16_t details_port) {
    client_port_ = client_port;
    query_fd_ = OpenUdp(query_port);
    details_fd_ = OpenUdp(details_port);
    if (query_fd_ < 0 || details_fd_ < 0) {
        LOG_ERROR("LanBeacon: cannot bind UDP %u/%u: %s", query_port, details_port, strerror(errno));
        return false;
    }
    LOG_INFO("LAN beacon: queries on UDP %u, announces to %u, details on %u",
             query_port, client_port, details_port);
    return true;
}

std::string LanBeacon::BuildAnnounce(const ServerInfo& info) {
    return std::to_string(kMagic) + "~0~" + info.ip + "~" + std::to_string(info.details_port) +
           "~" + kVersion;
}

std::vector<std::pair<std::string, std::string>> LanBeacon::Attributes(const ServerInfo& i) {
    // Order and the fixed values are the retail server's own reply (doc sec
    // 24); flags this server doesn't support (PunkBuster, VOIP) are "0".
    return {
        {"serverVersion", kVersion},
        {"serverIpAddress", i.ip},
        {"serverHostPort", std::to_string(i.game_port)},
        {"serverQueryPort", std::to_string(i.details_port)},
        {"serverPlayerCount", std::to_string(i.player_count)},
        {"serverAdminEmail", "N/A"},
        {"serverAdminName", "N/A"},
        {"serverBuildNumber", "00_101812"},
        {"serverFlagCheats", "0"},
        {"serverFlagCoop", "0"},
        {"serverFlagLeasedServer", "0"},
        {"serverFlagOfficialServer", "0"},
        {"serverFlagPassword", "0"},
        {"serverFlagPunkbuster", "0"},
        {"serverFlagSimunitions", "0"},
        {"serverFlagVOIP", "0"},
        {"serverGameName", "America's Army 3.3 Server"},
        {"serverHostName", i.host_name},
        {"serverHostNameEncoded", i.host_name},
        {"serverHostWebsite", "(unknown)"},
        {"serverHostingCompany", "(unknown)"},
        {"serverMapName", i.map_name},
        {"serverMaxPlayerExperience", "999999999"},
        {"serverMaxPlayers", std::to_string(i.max_players)},
        {"serverMaxSpectators", std::to_string(i.max_spectators)},
        {"serverMinPlayerExperience", "0"},
        {"serverMissionSize", i.mission_size},
        {"serverMissionTourID", i.mission_tour},
        {"serverMissionType", i.mission_type},
        {"serverMissionVariant", i.mission_variant},
        {"serverOperatingSystem", "Linux"},
        {"serverQualificationRequirements", "Any"},
        {"serverReputation", "3"},
        {"serverSkillRating", "0"},
        {"serverSpectators", "0"},
        {"serverTeamPlayers", "0"},
    };
}

uint16_t LanBeacon::FragmentChecksum(const uint8_t* d, size_t len) {
    // 30-byte table @0x631d9c05 ("l26aquiwy814m05kpt37vxc9osrnf\0").
    static const uint8_t kKey[30] = {'l','2','6','a','q','u','i','w','y','8','1','4','m','0','5',
                                     'k','p','t','3','7','v','x','c','9','o','s','r','n','f',0};
    uint8_t h[2] = {0xfe, 0x14};  // 0x14fe, little-endian
    if (len > 1010) len = 1010;   // max fragment payload (0x3f2)
    for (size_t i = 0; i < len; ++i) {
        uint8_t v = kKey[i % 30] ^ d[i];
        if (i) v ^= d[i - 1];
        h[i % 2] ^= v;
    }
    return static_cast<uint16_t>(h[0] | (h[1] << 8));
}

std::vector<uint8_t> LanBeacon::BuildStateReply(const ServerInfo& info) {
    auto attrs = Attributes(info);
    std::vector<uint8_t> body;
    PutKey(body, "attributeNames", 7);
    PutU16(body, static_cast<uint16_t>(attrs.size()));
    for (auto& a : attrs) PutStr(body, a.first);
    PutKey(body, "attributeValues", 7);
    PutU16(body, static_cast<uint16_t>(attrs.size()));
    for (auto& a : attrs) PutStr(body, a.second);
    PutKey(body, "resultCode", 2);
    PutU32(body, 1);

    // zlib level 9, as retail (0x78da header).
    uLongf clen = compressBound(body.size());
    std::vector<uint8_t> comp(clen);
    compress2(comp.data(), &clen, body.data(), body.size(), 9);
    comp.resize(clen);
    SscEncrypt(comp);

    std::vector<uint8_t> pkt;
    PutU16(pkt, 0x354a);
    PutU16(pkt, FragmentChecksum(comp.data(), comp.size()));
    PutU16(pkt, kMsgGetServerState | kCompressFlag);
    PutU16(pkt, 2);
    PutU16(pkt, 3);
    pkt.insert(pkt.end(), comp.begin(), comp.end());
    return pkt;
}

bool LanBeacon::ParseAcpu(const uint8_t* data, size_t len, AcpuMessage* out) {
    if (len < 10) return false;
    auto u16 = [](const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); };
    out->txid = data[0] | (data[1] << 8) | (data[2] << 16) | (static_cast<uint32_t>(data[3]) << 24);
    out->message_id = u16(data + 4);
    uint16_t fc = u16(data + 8);
    std::vector<uint8_t> body(data + 10, data + len);
    SscDecrypt(body);
    if (out->message_id & kCompressFlag) {
        std::vector<uint8_t> raw(64 * 1024);
        uLongf rl = raw.size();
        if (uncompress(raw.data(), &rl, body.data(), body.size()) != Z_OK) return false;
        raw.resize(rl);
        body.swap(raw);
    }
    size_t i = 0;
    auto need = [&](size_t n) { return i + n <= body.size(); };
    auto str = [&](std::string* s) {
        if (!need(2)) return false;
        uint16_t n = u16(&body[i]); i += 2;
        if (!need(n)) return false;
        s->assign(reinterpret_cast<const char*>(&body[i]), n && body[i + n - 1] == 0 ? n - 1 : n);
        i += n;
        return true;
    };
    out->fields.clear();
    for (uint16_t f = 0; f < fc; ++f) {
        AcpuField fl;
        if (!need(2)) return false;
        uint16_t kl = u16(&body[i]); i += 2;
        if (!need(kl + 1u)) return false;
        fl.key.assign(reinterpret_cast<const char*>(&body[i]), kl); i += kl;
        fl.type = body[i++];
        if (fl.type == 6) {
            if (!str(&fl.s)) return false;
        } else if (fl.type == 2) {
            if (!need(4)) return false;
            fl.i = static_cast<int32_t>(body[i] | (body[i + 1] << 8) | (body[i + 2] << 16) |
                                        (static_cast<uint32_t>(body[i + 3]) << 24));
            i += 4;
        } else if (fl.type == 7) {
            if (!need(2)) return false;
            uint16_t n = u16(&body[i]); i += 2;
            for (uint16_t k = 0; k < n; ++k) {
                std::string s;
                if (!str(&s)) return false;
                fl.arr.push_back(std::move(s));
            }
        } else {
            return false;  // type not used by this protocol
        }
        out->fields.push_back(std::move(fl));
    }
    return true;
}

std::string LanBeacon::DetectLanIp() {
    // connect() on UDP sends nothing; it just selects the outbound interface.
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    std::string ip = "127.0.0.1";
    if (fd < 0) return ip;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        sockaddr_in me{};
        socklen_t ml = sizeof(me);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&me), &ml) == 0) {
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf));
            ip = buf;
        }
    }
    close(fd);
    return ip;
}

void LanBeacon::Poll() {
    if (!info_) return;
    uint8_t buf[2048];
    sockaddr_in from{};
    socklen_t fl = sizeof(from);

    // Discovery queries -> announce, broadcast to the client listener port
    // and also straight back to the asker (same-host / no-broadcast setups).
    while (query_fd_ >= 0) {
        fl = sizeof(from);
        ssize_t n = recvfrom(query_fd_, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) break;
        buf[n] = 0;
        std::string msg(reinterpret_cast<char*>(buf), n);
        if (msg != std::to_string(kMagic) + "~1") continue;  // only queries; ignore others' announces
        std::string ann = BuildAnnounce(info_());
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(client_port_);
        dst.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(query_fd_, ann.data(), ann.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        dst.sin_addr = from.sin_addr;
        sendto(query_fd_, ann.data(), ann.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf));
        LOG_INFO("LAN beacon: query from %s:%u -> %s", ipbuf, ntohs(from.sin_port), ann.c_str());
    }

    // Details requests -> attribute reply to the requesting address.
    while (details_fd_ >= 0) {
        fl = sizeof(from);
        ssize_t n = recvfrom(details_fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) break;
        AcpuMessage req;
        if (!ParseAcpu(buf, static_cast<size_t>(n), &req) ||
            (req.message_id & 0xff) != kMsgGetServerState) {
            LOG_DEBUG("LAN beacon: ignoring non-get_server_state packet (%zd bytes)", n);
            continue;
        }
        std::string player;
        for (auto& f : req.fields) if (f.key == "playerName") player = f.s;
        auto reply = BuildStateReply(info_());
        sendto(details_fd_, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), fl);
        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf));
        LOG_INFO("LAN beacon: server-state request from %s (player '%s') -> %zu-byte reply",
                 ipbuf, player.c_str(), reply.size());
    }
}

}  // namespace network
}  // namespace aa3
