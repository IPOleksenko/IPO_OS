/*
 * hosting_server.c - Real Bare-Metal Web, Directory & File Hosting Server for IPO_OS
 *
 * Listens on 0.0.0.0 and arbitrary port (default 8080).
 * Accepts connections from ANY client IP address.
 * 
 * Features:
 *  - Browses directories (e.g. / or /applications) with greeting and system info.
 *  - Dynamically lists files and subfolders with clickable links.
 *  - Streams and downloads real files from the IPO_FS filesystem (binaries, text, etc.)
 *  - Self-expanding dynamic buffers (dyn_buf_t) for unlimited request/response sizes.
 *  - Fully interactive: clean exit on ESC or 'q'.
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
#include <driver/input/keymap/keymap.h>
#include <system/state.h>
#include <syscall.h>
#include <file_system/ipo_fs.h>
#include <memory/kmalloc.h>
#include <ioport.h>

static volatile bool server_running = true;

static bool check_user_interrupted(void) {
    if (!server_running || system_is_interrupted()) {
        server_running = false;
        return true;
    }
    keyboard_poll();
    uint8_t sc = keyboard_get_scancode();
    if (sc == 0x01 || sc == 0x10 || (sc == 0x2E && keyboard_is_ctrl_pressed())) { /* ESC, 'q', or Ctrl+C */
        system_request_interrupt();
        server_running = false;
        return true;
    }
    return false;
}

/* Parse HTTP request method and URI path */
static void parse_http_request(const char *req, char *method, size_t method_sz, char *path, size_t path_sz) {
    method[0] = '\0';
    path[0] = '\0';
    if (!req) return;

    while (*req == ' ' || *req == '\r' || *req == '\n') req++;

    size_t m = 0;
    while (*req && *req != ' ' && m + 1 < method_sz) {
        method[m++] = *req++;
    }
    method[m] = '\0';

    while (*req == ' ') req++;

    size_t p = 0;
    while (*req && *req != ' ' && *req != '\r' && *req != '\n' && *req != '?' && p + 1 < path_sz) {
        path[p++] = *req++;
    }
    path[p] = '\0';

    if (p == 0) {
        strncpy(path, "/", path_sz);
    }
}

/* Normalize requested URL path directly to real IPO_FS filesystem path (1:1 direct mapping, no shortcuts) */
static void map_url_to_fspath(const char *url_path, char *fs_path, size_t fs_path_sz) {
    if (!url_path || url_path[0] == '\0' || strcmp(url_path, "/") == 0) {
        strncpy(fs_path, "/", fs_path_sz);
        return;
    }

    /* Direct 1:1 mapping to filesystem path */
    if (url_path[0] != '/') {
        snprintf(fs_path, fs_path_sz, "/%s", url_path);
    } else {
        strncpy(fs_path, url_path, fs_path_sz);
    }

    /* Remove trailing slash if not root */
    size_t len = strlen(fs_path);
    if (len > 1 && fs_path[len - 1] == '/') {
        fs_path[len - 1] = '\0';
    }
}

/* Get parent URL path for directory navigation */
static void get_parent_url(const char *url, char *parent, size_t parent_sz) {
    strncpy(parent, url, parent_sz);
    size_t len = strlen(parent);
    if (len > 1 && parent[len - 1] == '/') {
        parent[len - 1] = '\0';
    }
    char *last = strrchr(parent, '/');
    if (!last || last == parent) {
        strncpy(parent, "/", parent_sz);
    } else {
        *last = '\0';
    }
}

static const char *get_filename(const char *path) {
    const char *last = strrchr(path, '/');
    return last ? (last + 1) : path;
}

static const char *get_mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html; charset=UTF-8";
    if (strcmp(dot, ".txt") == 0 || strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 ||
        strcmp(dot, ".asm") == 0 || strcmp(dot, ".s") == 0 || strcmp(dot, ".log") == 0)
        return "text/plain; charset=UTF-8";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".bin") == 0 || strcmp(dot, ".elf") == 0 ||
        strcmp(dot, ".img") == 0 || strcmp(dot, ".iso") == 0)
        return "application/octet-stream";
    return "application/octet-stream";
}

static void send_404(int client_fd, const char *path) {
    dyn_buf_t b;
    dyn_buf_init(&b, 512);
    dyn_buf_append_str(&b, "<!DOCTYPE html><html><head><meta charset='utf-8'><title>404 Not Found</title></head>");
    dyn_buf_append_str(&b, "<body style='font-family:monospace;background:#181818;color:#eee;padding:24px;'>");
    dyn_buf_append_str(&b, "<h1 style='color:#f85149;'>404 Not Found</h1>");
    dyn_buf_append_str(&b, "<p>Path <code>");
    dyn_buf_append_str(&b, path ? path : "");
    dyn_buf_append_str(&b, "</code> was not found on this IPO_OS host.</p>");
    dyn_buf_append_str(&b, "<hr style='border:none;border-top:1px solid #333;margin:20px 0;'>");
    dyn_buf_append_str(&b, "<p><a style='color:#58a6ff;' href='/'>⬅ Back to Root (/)</a></p>");
    dyn_buf_append_str(&b, "</body></html>\n");

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 404 Not Found\r\n"
             "Content-Type: text/html; charset=UTF-8\r\n"
             "Content-Length: %u\r\n"
             "Server: IPO_OS/1.0\r\n"
             "Connection: close\r\n\r\n",
             (unsigned int)b.size);
    send(client_fd, hdr, strlen(hdr), 0);
    send(client_fd, b.data, b.size, 0);
    dyn_buf_free(&b);
}

static void send_file(int client_fd, const char *fs_path, const struct ipo_inode *st) {
    int fd = ipo_open(fs_path);
    if (fd < 0) {
        send_404(client_fd, fs_path);
        return;
    }

    const char *mime = get_mime_type(fs_path);
    const char *fname = get_filename(fs_path);
    uint32_t file_size = (uint32_t)st->size;

    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %u\r\n"
             "Content-Disposition: inline; filename=\"%s\"\r\n"
             "Server: IPO_OS/1.0\r\n"
             "Connection: close\r\n\r\n",
             mime, file_size, fname);
    send(client_fd, hdr, strlen(hdr), 0);

    uint32_t offset = 0;
    char chunk[1024];
    while (offset < file_size) {
        if (check_user_interrupted()) {
            break;
        }
        uint32_t to_read = file_size - offset;
        if (to_read > sizeof(chunk)) to_read = sizeof(chunk);

        int r = ipo_read(fd, chunk, to_read, offset);
        if (r <= 0) break;

        send(client_fd, chunk, (size_t)r, 0);
        offset += (uint32_t)r;
        net_poll();
    }
    ipo_close(fd);
    printf("[file] Successfully streamed %u bytes of %s\n", offset, fs_path);
}

static void send_directory(int client_fd, const char *url_path, const char *fs_path,
                           const char *client_ip_str, uint16_t client_port, uint32_t request_counter) {
    dyn_buf_t body;
    dyn_buf_init(&body, 2048);

    /* 1. Header and greeting text */
    dyn_buf_append_str(&body, "<!DOCTYPE html>\n<html><head><meta charset='utf-8'>");
    dyn_buf_append_str(&body, "<title>IPO_OS Server - ");
    dyn_buf_append_str(&body, url_path);
    dyn_buf_append_str(&body, "</title>\n");
    dyn_buf_append_str(&body, "<style>\n"
                              "body { font-family: monospace; background: #181818; color: #e0e0e0; padding: 24px; line-height: 1.5; }\n"
                              "a { color: #58a6ff; text-decoration: none; }\n"
                              "a:hover { text-decoration: underline; }\n"
                              ".info { background: #222; border: 1px solid #333; border-radius: 6px; padding: 16px; margin-bottom: 24px; }\n"
                              "ul { list-style: none; padding: 0; margin: 0; }\n"
                              "li { padding: 8px 12px; border-bottom: 1px solid #282828; display: flex; align-items: center; }\n"
                              "li:hover { background: #252525; }\n"
                              ".tag { font-size: 11px; margin-left: 10px; color: #888; }\n"
                              ".down { color: #f0883e; font-size: 12px; margin-left: auto; text-decoration: none !important; }\n"
                              ".down:hover { text-decoration: underline !important; }\n"
                              "</style></head><body>\n");

    dyn_buf_append_str(&body, "<div class='info'>\n");
    dyn_buf_append_str(&body, "<h1 style='margin-top:0; color:#58a6ff;'>🚀 Hello from IPO_OS TCP/IP Stack!</h1>\n");
    dyn_buf_append_str(&body, "<p><b>OS Kernel:</b> IPO_OS (32-bit x86 Bare-Metal)</p>\n");
    dyn_buf_append_str(&body, "<p><b>Stack:</b> Custom RFC 793 / RFC 9293 TCP/IP stack built from scratch</p>\n");

    char info[256];
    snprintf(info, sizeof(info), "<p><b>Your Client Address:</b> %s:%u</p>\n", client_ip_str, client_port);
    dyn_buf_append_str(&body, info);

    snprintf(info, sizeof(info), "<p><b>Total Requests Served:</b> %u &nbsp;|&nbsp; <b>Kernel Uptime:</b> %u s</p>\n",
             request_counter, timer_seconds());
    dyn_buf_append_str(&body, info);
    dyn_buf_append_str(&body, "</div>\n");

    /* 2. Directory listing section */
    char dir_title[256];
    snprintf(dir_title, sizeof(dir_title), "<h2>📁 Directory: %s</h2>\n<div style='background:#1e1e1e;border:1px solid #333;border-radius:6px;'><ul>\n", url_path);
    dyn_buf_append_str(&body, dir_title);

    /* If not root, show Parent Directory link */
    if (strcmp(url_path, "/") != 0) {
        char parent[256];
        get_parent_url(url_path, parent, sizeof(parent));
        char p_link[512];
        snprintf(p_link, sizeof(p_link),
                 "<li><span style='margin-right:8px;'>📂</span><a style='color:#d2a8ff;font-weight:bold;' href='%s'>.. (Parent Directory)</a></li>\n",
                 parent);
        dyn_buf_append_str(&body, p_link);
    }

    /* Read directory entries */
    char *dir_buf = (char *)kmalloc(8192);
    int entries_found = 0;
    if (dir_buf) {
        int list_len = ipo_list_dir(fs_path, dir_buf, 8192);
        if (list_len > 0) {
            const char *p = dir_buf;
            char entry[128];
            while (*p) {
                const char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                if (len == 0) {
                    if (!nl) break;
                    p = nl + 1;
                    continue;
                }
                if (len >= sizeof(entry)) len = sizeof(entry) - 1;
                memcpy(entry, p, len);
                entry[len] = '\0';
                p = nl ? nl + 1 : p + len;

                bool is_dir = false;
                if (entry[len - 1] == '/') {
                    is_dir = true;
                    entry[len - 1] = '\0';
                }

                /* Construct URL and FS path for this entry */
                char child_url[256];
                if (strcmp(url_path, "/") == 0) {
                    snprintf(child_url, sizeof(child_url), "/%s", entry);
                } else {
                    snprintf(child_url, sizeof(child_url), "%s/%s", url_path, entry);
                }

                char child_fs[256];
                map_url_to_fspath(child_url, child_fs, sizeof(child_fs));

                struct ipo_inode c_st;
                memset(&c_st, 0, sizeof(c_st));
                if (ipo_stat(child_fs, &c_st) == 0 && (c_st.mode & IPO_INODE_TYPE_DIR)) {
                    is_dir = true;
                }

                char item_buf[512];
                if (is_dir) {
                    snprintf(item_buf, sizeof(item_buf),
                             "<li><span style='margin-right:8px;'>📁</span>"
                             "<a style='font-weight:bold;' href='%s'>%s/</a>"
                             "<span class='tag'>[DIR]</span></li>\n",
                             child_url, entry);
                } else {
                    uint32_t sz = (uint32_t)c_st.size;
                    snprintf(item_buf, sizeof(item_buf),
                             "<li><span style='margin-right:8px;'>📄</span>"
                             "<a href='%s'>%s</a>"
                             "<span class='tag'>(%u bytes)</span>"
                             "<a class='down' href='%s' download>[download]</a></li>\n",
                             child_url, entry, sz, child_url);
                }
                dyn_buf_append_str(&body, item_buf);
                entries_found++;
            }
        }
        kfree(dir_buf);
    }

    if (entries_found == 0) {
        dyn_buf_append_str(&body, "<li style='color:#777;padding:12px;'><i>(empty directory)</i></li>\n");
    }

    dyn_buf_append_str(&body, "</ul></div></body></html>\n");

    /* Send response with Content-Length */
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=UTF-8\r\n"
             "Content-Length: %u\r\n"
             "Server: IPO_OS/1.0\r\n"
             "Connection: close\r\n\r\n",
             (unsigned int)body.size);
    send(client_fd, hdr, strlen(hdr), 0);
    send(client_fd, body.data, body.size, 0);

    dyn_buf_free(&body);
}

int main(int argc, char **argv) {
    uint16_t port = 8080;
    if (argc > 1) {
        int p = atoi(argv[1]);
        if (p > 0 && p <= 65535) {
            port = (uint16_t)p;
        }
    }

    keyboard_set_app_input_mode(true);
    system_clear_interrupt();
    server_running = true;

    /* Dynamically query real device IP from the network subsystem */
    net_if_t *netif = net_get_interface();
    char ip_str[64] = "127.0.0.1";
    if (netif && netif->ip != 0) {
        ip_to_str(netif->ip, ip_str, sizeof(ip_str));
    }

    printf("============================================================\n");
    printf("   IPO_OS TCP Data & Web Hosting Server\n");
    printf("============================================================\n");
    printf("Device IP   : %s (link %s)\n", ip_str, (netif && netif->link_up) ? "UP" : "DOWN");
    printf("Listening on: 0.0.0.0:%u (INADDR_ANY)\n\n", port);
    printf("Open in any web browser or HTTP client:\n");
    printf("  Root directory : http://%s:%u/\n", ip_str, port);
    printf("  Applications   : http://%s:%u/applications\n\n", ip_str, port);
    printf("Press ESC, 'q', or Ctrl+C to stop server.\n\n");

    /* 1. Create stream socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        printf("[error] Failed to create socket\n");
        keyboard_set_app_input_mode(false);
        return 1;
    }

    /* 2. Bind to 0.0.0.0 and port */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("[error] Failed to bind to 0.0.0.0:%u\n", port);
        close(server_fd);
        keyboard_set_app_input_mode(false);
        return 1;
    }

    /* 3. Listen for incoming connections */
    if (listen(server_fd, 10) < 0) {
        printf("[error] Failed to listen on socket\n");
        close(server_fd);
        keyboard_set_app_input_mode(false);
        return 1;
    }

    printf("[server] Listening on port %u for incoming clients...\n", port);

    uint32_t request_counter = 0;
    uint32_t last_heartbeat = timer_millis();

    /* 4. Accept loop */
    while (!check_user_interrupted()) {
        net_poll();

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        /* Poll with 200ms timeout to keep keyboard and OS responsive */
        int client_fd = accept_timeout(server_fd, (struct sockaddr *)&client_addr, &client_len, 200);
        if (client_fd < 0) {
            if (check_user_interrupted()) {
                break;
            }
            if (timer_millis() - last_heartbeat >= 5000) {
                printf("[server] Active and listening on port %u (uptime %u s)...\n",
                       port, timer_seconds());
                last_heartbeat = timer_millis();
            }
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
            io_wait();
            continue;
        }

        request_counter++;
        char client_ip_str[64];
        ip_to_str(ntohl(client_addr.sin_addr.s_addr), client_ip_str, sizeof(client_ip_str));
        uint16_t client_port = ntohs(client_addr.sin_port);

        printf("\n[%u] Incoming client from %s:%u (socket fd %d)\n",
               request_counter, client_ip_str, client_port, client_fd);

        /* 5. Read client request into self-expanding dynamic buffer */
        dyn_buf_t req_buf;
        dyn_buf_init(&req_buf, 512);

        char temp_chunk[512];
        ssize_t bytes_read;
        uint32_t read_start = timer_millis();

        while (timer_millis() - read_start < 3000) {
            if (check_user_interrupted()) {
                break;
            }
            net_poll();
            bytes_read = recv(client_fd, temp_chunk, sizeof(temp_chunk) - 1, 0);
            if (bytes_read > 0) {
                temp_chunk[bytes_read] = '\0';
                dyn_buf_append(&req_buf, temp_chunk, (size_t)bytes_read);

                /* If HTTP header terminator reached */
                if (strstr((const char *)req_buf.data, "\r\n\r\n") != NULL ||
                    strstr((const char *)req_buf.data, "\n\n") != NULL ||
                    req_buf.size > 2048) {
                    break;
                }
            } else if (bytes_read <= 0) {
                break; /* EOF, interrupt, or error */
            }
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
            io_wait();
        }

        /* Null-terminate for request parsing */
        dyn_buf_append_byte(&req_buf, '\0');

        char method[16], url_path[256];
        parse_http_request((const char *)req_buf.data, method, sizeof(method), url_path, sizeof(url_path));
        printf("[http] %s %s\n", method, url_path);

        /* 6. Map URL path to IPO_FS filesystem path */
        char fs_path[256];
        map_url_to_fspath(url_path, fs_path, sizeof(fs_path));

        /* 7. Stat path in filesystem */
        struct ipo_inode st;
        memset(&st, 0, sizeof(st));
        int stat_res = ipo_stat(fs_path, &st);

        if (stat_res == 0 && (st.mode & IPO_INODE_TYPE_DIR)) {
            /* Requested a directory -> show greeting info + directory listing */
            send_directory(client_fd, url_path, fs_path, client_ip_str, client_port, request_counter);
        } else if (stat_res == 0 && (st.mode & IPO_INODE_TYPE_FILE)) {
            /* Requested a file -> stream and download file data */
            send_file(client_fd, fs_path, &st);
        } else {
            /* Check if root fallback */
            if (strcmp(fs_path, "/") == 0) {
                send_directory(client_fd, url_path, "/", client_ip_str, client_port, request_counter);
            } else {
                send_404(client_fd, url_path);
            }
        }

        /* 8. Close client connection and clean up */
        close(client_fd);
        dyn_buf_free(&req_buf);
    }

    printf("\n[server] Shutting down server socket...\n");
    close(server_fd);
    keyboard_set_app_input_mode(false);
    system_clear_interrupt();
    return 0;
}
