/* Hybrid FEC+ARQ receiver (C++ V1).
 *
 * Ports (127.0.0.1):
 *   bind 47002  <- media via relay
 *   send 47020  -> harness player (4B BE seq + 160B payload)
 *   send 47003  -> feedback to sender via relay
 *
 * Env: T0, DELAY_MS, DURATION_S
 */
#include "proto.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace proto;

struct Frame {
    bool have = false;
    bool delivered = false;
    uint8_t payload[PAYLOAD_BYTES]{};
};

struct Parity {
    bool have = false;
    uint8_t body[PAYLOAD_BYTES]{};
};

struct Gap {
    double first_seen = 0.0;
    bool nakked = false;
};

static int make_udp() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        std::exit(1);
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static void bind_port(int fd, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        perror("bind");
        std::exit(1);
    }
}

static sockaddr_in addr_port(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    return a;
}

struct Ctx {
    double t0 = 0.0;
    double delay_ms = 100.0;
    int in_fd = -1;
    int out_fd = -1;
    sockaddr_in player{};
    sockaddr_in relay_fb{};

    std::unordered_map<uint32_t, Frame> frames;
    std::unordered_map<uint32_t, Parity> parities;
    std::unordered_map<uint32_t, Gap> gaps;
    std::unordered_set<uint32_t> nakked;

    uint32_t highest_seen = 0;
    bool have_any = false;
};

static void deliver(Ctx& c, uint32_t seq, const uint8_t* payload) {
    Frame& f = c.frames[seq];
    if (!f.have) {
        f.have = true;
        std::memcpy(f.payload, payload, PAYLOAD_BYTES);
    }
    if (f.delivered) return;
    f.delivered = true;

    uint8_t pkt[HARNESS_PKT];
    write_be32(pkt, seq);
    std::memcpy(pkt + 4, f.payload, PAYLOAD_BYTES);
    sendto(c.out_fd, pkt, HARNESS_PKT, 0,
           reinterpret_cast<const sockaddr*>(&c.player), sizeof c.player);

    c.gaps.erase(seq);
}

static void try_fec(Ctx& c, uint32_t base) {
    auto pit = c.parities.find(base);
    if (pit == c.parities.end() || !pit->second.have) return;

    int missing = -1;
    int have_count = 0;
    for (int i = 0; i < FEC_K; ++i) {
        uint32_t s = base + uint32_t(i);
        auto it = c.frames.find(s);
        if (it != c.frames.end() && it->second.have) {
            ++have_count;
        } else {
            if (missing >= 0) return;  // more than one missing
            missing = i;
        }
    }
    if (missing < 0 || have_count != FEC_K - 1) return;

    uint8_t recovered[PAYLOAD_BYTES];
    std::memcpy(recovered, pit->second.body, PAYLOAD_BYTES);
    for (int i = 0; i < FEC_K; ++i) {
        if (i == missing) continue;
        xor_payload(recovered, c.frames[base + uint32_t(i)].payload);
    }
    deliver(c, base + uint32_t(missing), recovered);
}

static void send_nak(Ctx& c, uint32_t seq) {
    if (c.nakked.count(seq)) return;
    if (!useful_before_deadline(c.t0, c.delay_ms, seq)) return;
    uint8_t pkt[NAK_PKT];
    pkt[0] = TYPE_NAK;
    write_be32(pkt + 1, seq);
    sendto(c.out_fd, pkt, NAK_PKT, 0,
           reinterpret_cast<const sockaddr*>(&c.relay_fb), sizeof c.relay_fb);
    c.nakked.insert(seq);
}

static void note_gaps_and_nak(Ctx& c) {
    if (!c.have_any) return;
    const double now = now_s();

    // Mark gaps below highest_seen
    for (uint32_t s = 0; s < c.highest_seen; ++s) {
        auto it = c.frames.find(s);
        if (it != c.frames.end() && it->second.have) continue;
        Gap& g = c.gaps[s];
        if (g.first_seen == 0.0) g.first_seen = now;
    }

    // NAK after grace
    std::vector<uint32_t> to_nak;
    for (auto& kv : c.gaps) {
        uint32_t s = kv.first;
        Gap& g = kv.second;
        auto it = c.frames.find(s);
        if (it != c.frames.end() && it->second.have) continue;
        if (g.nakked) continue;
        if ((now - g.first_seen) * 1000.0 < NAK_GRACE_MS) continue;
        to_nak.push_back(s);
        g.nakked = true;
    }
    for (uint32_t s : to_nak)
        send_nak(c, s);
}

static void prune(Ctx& c) {
    // Drop frames far behind highest_seen to bound memory.
    if (!c.have_any) return;
    const uint32_t keep = uint32_t(SEQ_WINDOW);
    if (c.highest_seen < keep) return;
    uint32_t floor = c.highest_seen - keep;
    for (auto it = c.frames.begin(); it != c.frames.end();) {
        if (it->first < floor && it->second.delivered)
            it = c.frames.erase(it);
        else
            ++it;
    }
    for (auto it = c.parities.begin(); it != c.parities.end();) {
        if (it->first + uint32_t(FEC_K) < floor)
            it = c.parities.erase(it);
        else
            ++it;
    }
    for (auto it = c.gaps.begin(); it != c.gaps.end();) {
        if (it->first < floor)
            it = c.gaps.erase(it);
        else
            ++it;
    }
}

int main() {
    Ctx c;
    c.t0 = env_double("T0", 0.0);
    c.delay_ms = env_double("DELAY_MS", 100.0);

    c.in_fd = make_udp();
    bind_port(c.in_fd, 47002);

    c.out_fd = make_udp();
    c.player = addr_port(47020);
    c.relay_fb = addr_port(47003);

    uint8_t buf[2048];
    pollfd pfd{c.in_fd, POLLIN, 0};

    for (;;) {
        int pr = poll(&pfd, 1, NAK_GRACE_MS);
        if (pr < 0) continue;

        if (pfd.revents & POLLIN) {
            for (;;) {
                ssize_t n = recvfrom(c.in_fd, buf, sizeof buf, 0, nullptr, nullptr);
                if (n < 0) break;
                if (n < MEDIA_HDR) continue;

                uint8_t type = buf[0];
                uint32_t seq = read_be32(buf + 1);

                if (type == TYPE_DATA) {
                    if (n < MEDIA_PKT) continue;
                    const uint8_t* payload = buf + MEDIA_HDR;
                    Frame& f = c.frames[seq];
                    if (f.have) continue;  // duplicate
                    deliver(c, seq, payload);

                    if (!c.have_any || seq > c.highest_seen)
                        c.highest_seen = seq;
                    c.have_any = true;

                    try_fec(c, fec_base(seq));
                } else if (type == TYPE_PARITY) {
                    if (n < MEDIA_PKT) continue;
                    Parity& p = c.parities[seq];
                    if (p.have) continue;
                    p.have = true;
                    std::memcpy(p.body, buf + MEDIA_HDR, PAYLOAD_BYTES);
                    try_fec(c, seq);
                }
            }
        }

        note_gaps_and_nak(c);
        prune(c);
    }
    return 0;
}
