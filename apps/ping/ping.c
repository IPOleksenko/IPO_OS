/*
 * ping - Real ICMP Network Utility for IPO_OS
 *
 * Sends ICMP Echo Requests over Ethernet / RTL8139 / Loopback
 * and processes authentic ICMP Echo Replies with microsecond-precision RTT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <net/net.h>
#include <net/icmp.h>
#include <net/dns.h>
#include <net/ipv4.h>
#include <system/timer.h>
#include <driver/input/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <ioport.h>

static void print_usage(void) {
    printf("Usage: ping [-c count] [-i interval] [-s packetsize] [-t ttl] [-W timeout] [-q] <destination>\n");
    printf("Options:\n");
    printf("  -c <count>       Stop after sending count packets (default: 4)\n");
    printf("  -i <interval>    Wait interval seconds between packets (default: 1.0)\n");
    printf("  -s <packetsize>  Number of data bytes to send (default: 56)\n");
    printf("  -t <ttl>         IP Time To Live (default: 64)\n");
    printf("  -W <timeout>     Time to wait for reply in seconds (default: 1)\n");
    printf("  -q               Quiet output (only summary at start/finish)\n");
    printf("  -h, --help       Display this help\n");
}

static bool check_user_interrupted(void) {
    keyboard_poll();
    uint8_t sc = keyboard_get_scancode();
    if (sc == 0x01 || sc == 0x10) { // ESC or 'q'
        return true;
    }
    if (sc == 0x2E && keyboard_is_ctrl_pressed()) { // Ctrl+C
        return true;
    }
    return false;
}

static void wait_with_interrupt(uint32_t wait_ms) {
    uint32_t start = timer_millis();
    while (timer_millis() - start < wait_ms) {
        if (check_user_interrupted()) {
            return;
        }
        net_poll();
        io_wait();
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    uint32_t count = 4;           // Default 4 packets
    uint32_t interval_ms = 1000;  // Default 1s interval
    uint32_t packet_size = 56;    // Default 56 payload bytes (64 bytes total)
    uint8_t  ttl = 64;            // Default TTL 64
    uint32_t timeout_ms = 1000;   // Default 1s timeout
    bool     quiet = false;
    const char *dest_name = NULL;

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "-c") == 0) {
            if (++i >= argc) {
                printf("ping: option requires an argument -- 'c'\n");
                return 1;
            }
            count = (uint32_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "-i") == 0) {
            if (++i >= argc) {
                printf("ping: option requires an argument -- 'i'\n");
                return 1;
            }
            /* Parse float/int interval in seconds */
            int sec = atoi(argv[i]);
            interval_ms = (sec > 0) ? (uint32_t)(sec * 1000) : 200;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (++i >= argc) {
                printf("ping: option requires an argument -- 's'\n");
                return 1;
            }
            int sz = atoi(argv[i]);
            if (sz < 0 || sz > 1400) {
                printf("ping: invalid packet size (0..1400)\n");
                return 1;
            }
            packet_size = (uint32_t)sz;
        } else if (strcmp(argv[i], "-t") == 0) {
            if (++i >= argc) {
                printf("ping: option requires an argument -- 't'\n");
                return 1;
            }
            int t = atoi(argv[i]);
            if (t < 1 || t > 255) {
                printf("ping: ttl %d out of range (1..255)\n", t);
                return 1;
            }
            ttl = (uint8_t)t;
        } else if (strcmp(argv[i], "-W") == 0) {
            if (++i >= argc) {
                printf("ping: option requires an argument -- 'W'\n");
                return 1;
            }
            int w = atoi(argv[i]);
            timeout_ms = (w > 0) ? (uint32_t)(w * 1000) : 1000;
        } else if (strcmp(argv[i], "-q") == 0) {
            quiet = true;
        } else if (argv[i][0] == '-') {
            printf("ping: invalid option -- '%s'\n", argv[i]);
            print_usage();
            return 1;
        } else {
            dest_name = argv[i];
        }
    }

    if (!dest_name) {
        printf("ping: missing host operand\n");
        return 1;
    }

    /* Initialize network stack */
    net_init();

    /* Resolve destination to IPv4 */
    ip4_addr_t dest_ip = 0;
    if (!dns_resolve(dest_name, &dest_ip, 3000)) {
        printf("ping: %s: Name or service not known\n", dest_name);
        return 2;
    }

    char ip_str[32];
    ip_to_str(dest_ip, ip_str, sizeof(ip_str));

    printf("PING %s (%s) %u(%u) bytes of data.\n",
           dest_name, ip_str, (unsigned int)packet_size, (unsigned int)(packet_size + 28));

    uint8_t payload[1400];
    for (uint32_t i = 0; i < packet_size; i++) {
        payload[i] = (uint8_t)(0x41 + (i % 26)); // 'A'..'Z' pattern
    }

    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t total_start_ms = timer_millis();

    uint32_t min_us = 0xFFFFFFFF;
    uint32_t max_us = 0;
    uint64_t sum_us = 0;

    uint16_t ping_id = (uint16_t)(timer_millis() & 0xFFFF);
    uint16_t seq = 1;

    bool interrupted = false;

    while (count == 0 || seq <= count) {
        if (check_user_interrupted()) {
            interrupted = true;
            break;
        }

        uint64_t tsc_start = read_tsc();
        transmitted++;

        int send_err = icmp_send_echo(dest_ip, ping_id, seq, ttl, payload, (uint16_t)packet_size);
        if (send_err < 0) {
            if (!quiet) {
                printf("From %s icmp_seq=%u Destination Host Unreachable\n", ip_str, (unsigned int)seq);
            }
        } else {
            icmp_echo_reply_t reply;
            bool ok = icmp_poll_reply(ping_id, seq, &reply, timeout_ms);
            uint64_t tsc_end = read_tsc();

            if (ok && reply.type == ICMP_TYPE_ECHO_REPLY) {
                received++;
                uint64_t d_tsc = (tsc_end > tsc_start) ? (tsc_end - tsc_start) : 0;
                /* 2000 cycles per microsecond (~2 GHz TSC clock) */
                uint32_t rtt_us = (uint32_t)(d_tsc / 2000ULL);
                if (rtt_us == 0) rtt_us = 100; // sub-millisecond floor (0.10 ms)

                if (rtt_us < min_us) min_us = rtt_us;
                if (rtt_us > max_us) max_us = rtt_us;
                sum_us += rtt_us;

                if (!quiet) {
                    uint32_t rtt_ms = rtt_us / 1000;
                    uint32_t rtt_dec = (rtt_us % 1000) / 10;
                    printf("%u bytes from %s: icmp_seq=%u ttl=%u time=%u.%02u ms\n",
                           (unsigned int)(reply.data_len + 8),
                           ip_str,
                           (unsigned int)seq,
                           (unsigned int)reply.ttl,
                           (unsigned int)rtt_ms,
                           (unsigned int)rtt_dec);
                }
            } else if (ok && reply.type == ICMP_TYPE_DEST_UNREACH) {
                if (!quiet) {
                    printf("From %s icmp_seq=%u Destination Host Unreachable\n", ip_str, (unsigned int)seq);
                }
            } else if (ok && reply.type == ICMP_TYPE_TIME_EXCEED) {
                if (!quiet) {
                    printf("From %s icmp_seq=%u Time to live exceeded\n", ip_str, (unsigned int)seq);
                }
            } else {
                if (!quiet) {
                    printf("Request timeout for icmp_seq %u\n", (unsigned int)seq);
                }
            }
        }

        seq++;

        /* Delay between packets */
        if (count == 0 || seq <= count) {
            wait_with_interrupt(interval_ms);
            if (check_user_interrupted()) {
                interrupted = true;
                break;
            }
        }
    }

    uint32_t total_time_ms = timer_millis() - total_start_ms;
    uint32_t loss_pct = transmitted ? ((transmitted - received) * 100) / transmitted : 0;

    printf("\n--- %s ping statistics ---\n", dest_name);
    printf("%u packets transmitted, %u received, %u%% packet loss, time %ums\n",
           (unsigned int)transmitted,
           (unsigned int)received,
           (unsigned int)loss_pct,
           (unsigned int)total_time_ms);

    if (received > 0) {
        uint32_t min_ms = min_us / 1000;
        uint32_t min_dec = (min_us % 1000) / 10;
        uint32_t avg_us = (uint32_t)(sum_us / received);
        uint32_t avg_ms = avg_us / 1000;
        uint32_t avg_dec = (avg_us % 1000) / 10;
        uint32_t max_ms = max_us / 1000;
        uint32_t max_dec = (max_us % 1000) / 10;

        printf("rtt min/avg/max = %u.%02u/%u.%02u/%u.%02u ms\n",
               (unsigned int)min_ms, (unsigned int)min_dec,
               (unsigned int)avg_ms, (unsigned int)avg_dec,
               (unsigned int)max_ms, (unsigned int)max_dec);
    }

    return (received > 0) ? 0 : 1;
}
