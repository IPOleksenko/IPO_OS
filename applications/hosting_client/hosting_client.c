/*
 * hosting_client.c - Bare-Metal TCP/HTTP Data Fetcher & Client for IPO_OS
 *
 * Connects to a remote host (supports arbitrary string length for IPs / hostnames,
 * including 100+ character FQDNs).
 * Sends request and dynamically accumulates the entire response stream into
 * self-expanding dynamic buffers (dyn_buf_t) without truncation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <net/socket.h>
#include <net/net.h>
#include <net/dyn_buf.h>
#include <system/timer.h>
#include <driver/input/keyboard.h>
#include <system/state.h>
#include <syscall.h>
#include <ioport.h>

static void print_usage(void) {
    printf("Usage: hosting_client <host_or_ip> [port] [path]\n");
    printf("Examples:\n");
    printf("  hosting_client 10.0.2.2 8080 /\n");
    printf("  hosting_client 127.0.0.1 8080 /index.html\n");
    printf("  hosting_client a-very-long-hostname-with-arbitrary-characters-example.local 80 /\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    /* Support arbitrary-length host/IP strings (even 100+ chars) */
    dyn_str_t host_str;
    dyn_str_init(&host_str, argv[1]);

    uint16_t port = 8080;
    if (argc > 2) {
        int p = atoi(argv[2]);
        if (p > 0 && p <= 65535) {
            port = (uint16_t)p;
        }
    }

    const char *path = (argc > 3) ? argv[3] : "/";

    printf("============================================================\n");
    printf("   IPO_OS Bare-Metal TCP Data Fetching Client\n");
    printf("============================================================\n");
    printf("Target Host : %s\n", dyn_str_c(&host_str));
    printf("Target Port : %u\n", port);
    printf("Request Path: %s\n\n", path);

    /* 1. Create client socket */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("[error] Failed to create socket\n");
        dyn_str_free(&host_str);
        return 1;
    }

    /* 2. Connect to remote endpoint */
    printf("[client] Connecting to %s:%u...\n", dyn_str_c(&host_str), port);
    int res = connect_endpoint(sock, dyn_str_c(&host_str), port);
    if (res < 0) {
        printf("[error] Connection failed to %s:%u\n", dyn_str_c(&host_str), port);
        close(sock);
        dyn_str_free(&host_str);
        return 1;
    }

    printf("[client] Connected successfully!\n");

    /* 3. Build HTTP request in dynamic buffer */
    dyn_buf_t req_buf;
    dyn_buf_init(&req_buf, 256);

    dyn_buf_append_str(&req_buf, "GET ");
    dyn_buf_append_str(&req_buf, path);
    dyn_buf_append_str(&req_buf, " HTTP/1.1\r\nHost: ");
    dyn_buf_append_str(&req_buf, dyn_str_c(&host_str));
    dyn_buf_append_str(&req_buf, "\r\nUser-Agent: IPO_OS-Client/1.0\r\nConnection: close\r\n\r\n");

    printf("[client] Sending request (%u bytes):\n%s\n",
           (unsigned int)req_buf.size, (char *)req_buf.data);

    ssize_t sent = send(sock, req_buf.data, req_buf.size, 0);
    dyn_buf_free(&req_buf);

    if (sent < 0) {
        printf("[error] Failed to send request\n");
        close(sock);
        dyn_str_free(&host_str);
        return 1;
    }

    /* 4. Stream response into self-expanding dynamic buffer */
    printf("[client] Receiving data stream from server...\n");

    dyn_buf_t resp_buf;
    dyn_buf_init(&resp_buf, 1024);

    char chunk[512];
    ssize_t bytes_recv;
    uint32_t start_time = timer_millis();

    while (true) {
        net_poll();
        bytes_recv = recv(sock, chunk, sizeof(chunk), 0);

        if (bytes_recv > 0) {
            /* Append chunk to self-expanding dynamic buffer */
            dyn_buf_append(&resp_buf, chunk, (size_t)bytes_recv);
        } else if (bytes_recv == 0) {
            /* Server closed connection (EOF) */
            printf("[client] Server closed connection (EOF reached)\n");
            break;
        } else {
            /* Error or timeout */
            if (timer_millis() - start_time > 10000) {
                printf("[warning] Read timeout reached\n");
                break;
            }
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
            io_wait();
        }
    }

    uint32_t elapsed_ms = timer_millis() - start_time;

    /* 5. Print received data */
    dyn_buf_append_byte(&resp_buf, '\0');

    printf("\n------------------- [ RECEIVED DATA ] -------------------\n");
    printf("%s\n", (char *)resp_buf.data);
    printf("---------------------------------------------------------\n");
    printf("[stats] Downloaded : %u bytes\n", (unsigned int)resp_buf.size - 1);
    printf("[stats] Elapsed    : %u ms\n", elapsed_ms);
    printf("=========================================================\n");

    /* 6. Clean up */
    close(sock);
    dyn_buf_free(&resp_buf);
    dyn_str_free(&host_str);

    return 0;
}

