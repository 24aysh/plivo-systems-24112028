/* Hybrid FEC+ARQ sender (C++ V1).
 *
 * Ports (127.0.0.1):
 *   bind 47010  <- harness source (4B BE seq + 160B payload)
 *   send 47001  -> relay uplink
 *   bind 47004  <- feedback from receiver via relay
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
#include <unordered_set>

using namespace proto;

struct Slot {
    bool valid = false;
    uint32_t seq = 0;
    uint8_t payload[PAYLOAD_BYTES]{};
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

static void send_media(int fd, const sockaddr_in& dest, uint8_t type,
                       uint32_t seq, const uint8_t* body) {
    uint8_t pkt[MEDIA_PKT];
    pkt[0] = type;
    write_be32(pkt + 1, seq);
    std::memcpy(pkt + MEDIA_HDR, body, PAYLOAD_BYTES);
    sendto(fd, pkt, MEDIA_PKT, 0, reinterpret_cast<const sockaddr*>(&dest),
           sizeof dest);
}

int main() {
    const double t0 = env_double("T0", 0.0);
    const double delay_ms = env_double("DELAY_MS", 100.0);

    int src_fd = make_udp();
    bind_port(src_fd, 47010);

    int fb_fd = make_udp();
    bind_port(fb_fd, 47004);

    int out_fd = make_udp();
    sockaddr_in relay = addr_port(47001);

    Slot ring[SEQ_WINDOW];
    std::unordered_set<uint32_t> retransmitted;

    uint8_t xor_acc[PAYLOAD_BYTES]{};
    int xor_count = 0;
    uint32_t xor_base = 0;
    bool xor_active = false;

    uint8_t buf[2048];
    pollfd pfds[2] = {
        {src_fd, POLLIN, 0},
        {fb_fd, POLLIN, 0},
    };

    for (;;) {
        int pr = poll(pfds, 2, 50);
        if (pr < 0) continue;

        // --- source frames ---
        if (pfds[0].revents & POLLIN) {
            for (;;) {
                ssize_t n = recvfrom(src_fd, buf, sizeof buf, 0, nullptr, nullptr);
                if (n < 0) break;
                if (n < HARNESS_PKT) continue;

                uint32_t seq = read_be32(buf);
                const uint8_t* payload = buf + 4;

                Slot& slot = ring[seq % SEQ_WINDOW];
                slot.valid = true;
                slot.seq = seq;
                std::memcpy(slot.payload, payload, PAYLOAD_BYTES);

                send_media(out_fd, relay, TYPE_DATA, seq, payload);

                // FEC group accumulation
                uint32_t base = fec_base(seq);
                if (!xor_active || xor_base != base) {
                    std::memset(xor_acc, 0, PAYLOAD_BYTES);
                    xor_count = 0;
                    xor_base = base;
                    xor_active = true;
                }
                xor_payload(xor_acc, payload);
                ++xor_count;
                if (xor_count == FEC_K) {
                    send_media(out_fd, relay, TYPE_PARITY, xor_base, xor_acc);
                    xor_active = false;
                    xor_count = 0;
                }
            }
        }

        // --- NAK feedback ---
        if (pfds[1].revents & POLLIN) {
            for (;;) {
                ssize_t n = recvfrom(fb_fd, buf, sizeof buf, 0, nullptr, nullptr);
                if (n < 0) break;
                if (n < NAK_PKT) continue;
                if (buf[0] != TYPE_NAK) continue;

                uint32_t seq = read_be32(buf + 1);
                if (retransmitted.count(seq)) continue;
                if (!useful_before_deadline(t0, delay_ms, seq)) continue;

                Slot& slot = ring[seq % SEQ_WINDOW];
                if (!slot.valid || slot.seq != seq) continue;

                send_media(out_fd, relay, TYPE_DATA, seq, slot.payload);
                retransmitted.insert(seq);
            }
        }
    }
    return 0;
}
