/*
 * hosting_server.c - Full-Featured Bare-Metal Web & HTTP Server for IPO_OS
 *
 * Listens on 0.0.0.0 and arbitrary port (default 8080).
 * Accepts connections from ANY client IP address.
 * 
 * Implemented HTTP/1.1 Methods:
 *  - GET     : Browse directories, stream & download files with MIME detection.
 *  - HEAD    : Retrieve exact resource headers, size & metadata without body.
 *  - POST    : Upload files via browser multipart/form-data or binary payload.
 *  - PUT     : Create or replace files at target path with raw body data.
 *  - DELETE  : Delete files/directories from IPO_FS with 403 root protection.
 *  - OPTIONS : CORS pre-flight & allowed HTTP methods inspection (Allow header).
 *  - PATCH   : Partial resource update / append data at offset or end of file.
 *  - TRACE   : Diagnostic application-level echo of received request headers.
 *  - Others  : 405 Method Not Allowed with Allow header according to RFC 7231.
 *
 * Interactive:
 *  - Clean exit on ESC, 'q', or Ctrl+C.
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

/* Case-insensitive substring search */
static const char *my_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n) {
            char ch1 = *h;
            char ch2 = *n;
            if (ch1 >= 'A' && ch1 <= 'Z') ch1 += ('a' - 'A');
            if (ch2 >= 'A' && ch2 <= 'Z') ch2 += ('a' - 'A');
            if (ch1 != ch2) break;
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

/* Binary-safe memory pattern search */
static const uint8_t *mem_find(const uint8_t *haystack, size_t haystack_len,
                               const uint8_t *needle, size_t needle_len) {
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len) return NULL;
    size_t limit = haystack_len - needle_len;
    for (size_t i = 0; i <= limit; i++) {
        if (haystack[i] == needle[0]) {
            if (memcmp(haystack + i, needle, needle_len) == 0) {
                return haystack + i;
            }
        }
    }
    return NULL;
}

/* Parse HTTP request method, URI path, and query string */
static void parse_http_request(const char *req, char *method, size_t method_sz,
                               char *path, size_t path_sz, char *query, size_t query_sz) {
    method[0] = '\0';
    path[0] = '\0';
    if (query && query_sz > 0) query[0] = '\0';
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

    if (*req == '?') {
        req++;
        size_t q = 0;
        while (*req && *req != ' ' && *req != '\r' && *req != '\n' && query && q + 1 < query_sz) {
            query[q++] = *req++;
        }
        if (query && q < query_sz) query[q] = '\0';
    }

    if (p == 0) {
        strncpy(path, "/", path_sz);
    }
}

/* Parse Content-Length header */
static long parse_content_length(const char *headers, size_t hdr_len) {
    const char *p = my_strcasestr(headers, "Content-Length:");
    if (!p || (size_t)(p - headers) >= hdr_len) return -1;
    p += 15;
    while (*p == ' ' || *p == '\t') p++;
    long len = 0;
    while (*p >= '0' && *p <= '9') {
        len = len * 10 + (*p - '0');
        p++;
    }
    return len;
}

/* Parse Content-Type header */
static void parse_content_type(const char *headers, size_t hdr_len, char *ctype, size_t ctype_sz) {
    ctype[0] = '\0';
    const char *p = my_strcasestr(headers, "Content-Type:");
    if (!p || (size_t)(p - headers) >= hdr_len) return;
    p += 13;
    while (*p == ' ' || *p == '\t') p++;
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && i + 1 < ctype_sz) {
        ctype[i++] = *p++;
    }
    ctype[i] = '\0';
}

/* Sanitize filename: strip paths, slashes, forbidden dots */
static void sanitize_filename(char *filename, size_t sz) {
    if (!filename || sz == 0) return;
    char *last_slash = strrchr(filename, '/');
    char *last_bslash = strrchr(filename, '\\');
    char *base = filename;
    if (last_slash && last_slash + 1 > base) base = last_slash + 1;
    if (last_bslash && last_bslash + 1 > base) base = last_bslash + 1;
    if (base != filename) {
        memmove(filename, base, strlen(base) + 1);
    }
    /* Trim quotes or spaces */
    size_t len = strlen(filename);
    while (len > 0 && (filename[len - 1] == ' ' || filename[len - 1] == '\"' || filename[len - 1] == '\'')) {
        filename[--len] = '\0';
    }
    if (filename[0] == '\"' || filename[0] == '\'') {
        memmove(filename, filename + 1, len);
    }
    if (filename[0] == '\0' || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
        strncpy(filename, "upload.bin", sz);
    }
}

/* Normalize requested URL path directly to real IPO_FS filesystem path */
static void map_url_to_fspath(const char *url_path, char *fs_path, size_t fs_path_sz) {
    if (!url_path || url_path[0] == '\0' || strcmp(url_path, "/") == 0) {
        strncpy(fs_path, "/", fs_path_sz);
        return;
    }

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

/* 404 Not Found response */
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

/* 405 Method Not Allowed response */
static void send_method_not_allowed(int client_fd, const char *method) {
    const char body[] = "405 Method Not Allowed\nAllowed methods: GET, HEAD, POST, PUT, DELETE, OPTIONS, PATCH, TRACE\n";
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 405 Method Not Allowed\r\n"
             "Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS, PATCH, TRACE\r\n"
             "Content-Type: text/plain; charset=UTF-8\r\n"
             "Content-Length: %u\r\n"
             "Server: IPO_OS/1.0\r\n"
             "Connection: close\r\n\r\n",
             (unsigned int)strlen(body));
    send(client_fd, hdr, strlen(hdr), 0);
    send(client_fd, body, strlen(body), 0);
    printf("[http] 405 Method Not Allowed for '%s'\n", method ? method : "UNKNOWN");
}

/* GET: Stream file to client */
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
    char chunk[2048];
    while (offset < file_size) {
        if (check_user_interrupted()) {
            break;
        }
        uint32_t to_read = file_size - offset;
        if (to_read > sizeof(chunk)) to_read = sizeof(chunk);

        int r = ipo_read(fd, chunk, to_read, offset);
        if (r <= 0) break;

        size_t sent_tot = 0;
        while (sent_tot < (size_t)r) {
            if (check_user_interrupted()) break;
            ssize_t s = send(client_fd, chunk + sent_tot, (size_t)r - sent_tot, 0);
            if (s <= 0) break;
            sent_tot += (size_t)s;
            net_poll();
        }
        if (sent_tot < (size_t)r) {
            printf("[file] Send aborted or failed for %s at %u bytes\n", fs_path, offset + (uint32_t)sent_tot);
            break;
        }

        offset += (uint32_t)r;
        net_poll();
    }
    ipo_close(fd);
    printf("[file] Successfully streamed %u bytes of %s\n", offset, fs_path);
}

/* HEAD: Return headers only without body */
static void send_head(int client_fd, const char *url_path, const char *fs_path) {
    struct ipo_inode st;
    memset(&st, 0, sizeof(st));
    int stat_res = ipo_stat(fs_path, &st);

    if (stat_res == 0 && (st.mode & IPO_INODE_TYPE_FILE)) {
        const char *mime = get_mime_type(fs_path);
        const char *fname = get_filename(fs_path);
        uint32_t file_size = (uint32_t)st.size;

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
        printf("[head] File %s (%u bytes)\n", fs_path, file_size);
    } else if (strcmp(fs_path, "/") == 0 || (stat_res == 0 && (st.mode & IPO_INODE_TYPE_DIR))) {
        char hdr[512];
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=UTF-8\r\n"
                 "Server: IPO_OS/1.0\r\n"
                 "Connection: close\r\n\r\n");
        send(client_fd, hdr, strlen(hdr), 0);
        printf("[head] Directory %s\n", fs_path);
    } else {
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 404 Not Found\r\n"
                 "Content-Type: text/html; charset=UTF-8\r\n"
                 "Server: IPO_OS/1.0\r\n"
                 "Connection: close\r\n\r\n");
        send(client_fd, hdr, strlen(hdr), 0);
        printf("[head] 404 Not Found %s\n", url_path);
    }
}

/* OPTIONS: Pre-flight & method query */
static void handle_options(int client_fd, const char *url_path) {
    (void)url_path;
    const char resp[] =
        "HTTP/1.1 204 No Content\r\n"
        "Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS, PATCH, TRACE\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, HEAD, POST, PUT, DELETE, OPTIONS, PATCH, TRACE\r\n"
        "Access-Control-Allow-Headers: Content-Type, Content-Length, X-Requested-With, X-Offset\r\n"
        "Server: IPO_OS/1.0\r\n"
        "Connection: close\r\n\r\n";
    send(client_fd, resp, strlen(resp), 0);
    printf("[options] Sent Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS, PATCH, TRACE\n");
}

/* TRACE: Echo back received request */
static void handle_trace(int client_fd, const dyn_buf_t *req_buf) {
    size_t echo_len = req_buf ? req_buf->size : 0;
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: message/http\r\n"
             "Content-Length: %u\r\n"
             "Server: IPO_OS/1.0\r\n"
             "Connection: close\r\n\r\n",
             (unsigned int)echo_len);
    send(client_fd, hdr, strlen(hdr), 0);
    if (echo_len > 0) {
        send(client_fd, req_buf->data, echo_len, 0);
    }
    printf("[trace] Echoed %u bytes of request\n", (unsigned int)echo_len);
}

/* DELETE: Remove file or directory */
static void handle_delete(int client_fd, const char *url_path, const char *fs_path) {
    if (strcmp(fs_path, "/") == 0 || strcmp(url_path, "/") == 0) {
        const char resp[] =
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "Content-Length: 32\r\n"
            "Server: IPO_OS/1.0\r\n"
            "Connection: close\r\n\r\n"
            "Forbidden: Cannot delete root /\n";
        send(client_fd, resp, strlen(resp), 0);
        printf("[delete] Refused deletion of root /\n");
        return;
    }

    struct ipo_inode st;
    memset(&st, 0, sizeof(st));
    if (ipo_stat(fs_path, &st) != 0) {
        send_404(client_fd, url_path);
        printf("[delete] Path not found: %s\n", fs_path);
        return;
    }

    int del_res = ipo_delete(fs_path);
    if (del_res != 0) {
        char err_body[256];
        snprintf(err_body, sizeof(err_body), "Failed to delete '%s' (code %d)\n", fs_path, del_res);
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 500 Internal Server Error\r\n"
                 "Content-Type: text/plain; charset=UTF-8\r\n"
                 "Content-Length: %u\r\n"
                 "Server: IPO_OS/1.0\r\n"
                 "Connection: close\r\n\r\n",
                 (unsigned int)strlen(err_body));
        send(client_fd, hdr, strlen(hdr), 0);
        send(client_fd, err_body, strlen(err_body), 0);
        printf("[delete] Failed to delete %s (code %d)\n", fs_path, del_res);
        return;
    }

    printf("[delete] Successfully deleted '%s'\n", fs_path);

    dyn_buf_t b;
    dyn_buf_init(&b, 512);
    dyn_buf_append_str(&b, "<!DOCTYPE html><html><head><meta charset='utf-8'>");
    
    char parent[256];
    get_parent_url(url_path, parent, sizeof(parent));
    char refresh_tag[256];
    snprintf(refresh_tag, sizeof(refresh_tag), "<meta http-equiv='refresh' content='1;url=%s'>", parent);
    dyn_buf_append_str(&b, refresh_tag);

    dyn_buf_append_str(&b, "<title>Deleted</title>\n"
                           "<style>body{font-family:monospace;background:#181818;color:#eee;padding:24px;text-align:center;}"
                           ".card{background:#222;border:1px solid #30363d;border-radius:8px;max-width:500px;margin:40px auto;padding:24px;}"
                           "a{color:#58a6ff;font-weight:bold;text-decoration:none;}</style></head><body>"
                           "<div class='card'><h2 style='color:#3fb950;margin-top:0;'>✔ Deleted Successfully</h2>");
    char msg[512];
    snprintf(msg, sizeof(msg),
             "<p>Resource <code><b>%s</b></code> was deleted from IPO_FS.</p>"
             "<p style='color:#888;'>Redirecting to parent directory...</p>"
             "<p><a href='%s'>⬅ Return to %s</a></p></div></body></html>\n",
             url_path, parent, parent);
    dyn_buf_append_str(&b, msg);

    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=UTF-8\r\n"
             "Content-Length: %u\r\n"
             "Server: IPO_OS/1.0\r\n"
             "Connection: close\r\n\r\n",
             (unsigned int)b.size);
    send(client_fd, hdr, strlen(hdr), 0);
    send(client_fd, b.data, b.size, 0);
    dyn_buf_free(&b);
}

/* PATCH: Partial modification or appending to resource, streamed directly */
static void handle_patch_stream(int client_fd, const char *url_path, const char *fs_path,
                                const char *query, long content_len,
                                const uint8_t *initial_body, size_t initial_body_len) {
    struct ipo_inode st;
    memset(&st, 0, sizeof(st));
    if (ipo_stat(fs_path, &st) != 0 || !(st.mode & IPO_INODE_TYPE_FILE)) {
        send_404(client_fd, url_path);
        return;
    }

    uint32_t current_size = (uint32_t)st.size;
    uint32_t target_offset = current_size; /* Default: append to end */

    if (query) {
        const char *opos = my_strcasestr(query, "offset=");
        if (opos) {
            target_offset = (uint32_t)atoi(opos + 7);
        }
    }

    int fd = ipo_open(fs_path);
    if (fd < 0) {
        char err[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 26\r\nConnection: close\r\n\r\nFailed to open file\n";
        send(client_fd, err, strlen(err), 0);
        return;
    }

    uint32_t written = 0;
    if (initial_body && initial_body_len > 0) {
        size_t to_write = initial_body_len;
        if (content_len >= 0 && (long)to_write > content_len) {
            to_write = (size_t)content_len;
        }
        ipo_write(fd, initial_body, (uint32_t)to_write, target_offset);
        written += (uint32_t)to_write;
    }

    char temp_chunk[2048];
    uint32_t last_activity = timer_millis();

    while (!check_user_interrupted()) {
        if (content_len >= 0 && (long)written >= content_len) {
            break;
        }
        size_t to_recv = sizeof(temp_chunk);
        if (content_len >= 0) {
            size_t rem = (size_t)content_len - written;
            if (rem < to_recv) to_recv = rem;
        }
        if (to_recv == 0) break;

        net_poll();
        ssize_t n = recv(client_fd, temp_chunk, to_recv, 0);
        if (n > 0) {
            ipo_write(fd, temp_chunk, (uint32_t)n, target_offset + written);
            written += (uint32_t)n;
            last_activity = timer_millis();
        } else if (n == 0) {
            break;
        } else {
            if (timer_millis() - last_activity > 8000) {
                printf("[patch] Timeout after %u bytes\n", written);
                break;
            }
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
            io_wait();
        }
    }

    ipo_close(fd);
    printf("[patch] Patched %u bytes to '%s' at offset %u\n", written, fs_path, target_offset);

    char resp_body[256];
    snprintf(resp_body, sizeof(resp_body),
             "Successfully patched %u bytes at offset %u in %s\n",
             written, target_offset, fs_path);
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/plain; charset=UTF-8\r\n"
             "Content-Length: %u\r\n"
             "Server: IPO_OS/1.0\r\n"
             "Connection: close\r\n\r\n",
             (unsigned int)strlen(resp_body));
    send(client_fd, hdr, strlen(hdr), 0);
    send(client_fd, resp_body, strlen(resp_body), 0);
}

static void send_upload_success(int client_fd, const char *redirect_url, const char *filename, uint32_t bytes_written) {
    dyn_buf_t b;
    dyn_buf_init(&b, 1024);
    dyn_buf_append_str(&b, "<!DOCTYPE html>\n<html><head><meta charset='utf-8'>");
    
    char refresh_tag[256];
    snprintf(refresh_tag, sizeof(refresh_tag), "<meta http-equiv='refresh' content='1;url=%s'>", redirect_url);
    dyn_buf_append_str(&b, refresh_tag);
    
    dyn_buf_append_str(&b, "<title>Upload Successful</title>\n"
                           "<style>\n"
                           "body { font-family: monospace; background: #181818; color: #eee; padding: 24px; text-align: center; }\n"
                           ".card { background: #222; border: 1px solid #30363d; border-radius: 8px; max-width: 520px; margin: 40px auto; padding: 28px; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }\n"
                           "a { color: #58a6ff; text-decoration: none; font-weight: bold; }\n"
                           "a:hover { text-decoration: underline; }\n"
                           "</style></head><body>\n");
    dyn_buf_append_str(&b, "<div class='card'>\n");
    dyn_buf_append_str(&b, "<h2 style='color:#3fb950;margin-top:0;'>✔ File Saved Successfully!</h2>\n");
    
    char msg[512];
    snprintf(msg, sizeof(msg),
             "<p>Saved file <code><b>%s</b></code> (<b>%u</b> bytes) into current directory.</p>\n"
             "<p style='color:#888;margin-top:16px;'>Redirecting back to folder in 1 second...</p>\n"
             "<p style='margin-top:20px;'><a href='%s'>⬅ Return to Directory Listing</a></p>\n",
             filename, bytes_written, redirect_url);
    dyn_buf_append_str(&b, msg);
    dyn_buf_append_str(&b, "</div></body></html>\n");

    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 303 See Other\r\n"
             "Location: %s\r\n"
             "Content-Type: text/html; charset=UTF-8\r\n"
             "Content-Length: %u\r\n"
             "Server: IPO_OS/1.0\r\n"
             "Connection: close\r\n\r\n",
             redirect_url, (unsigned int)b.size);
    send(client_fd, hdr, strlen(hdr), 0);
    send(client_fd, b.data, b.size, 0);
    dyn_buf_free(&b);
}

static void send_upload_error(int client_fd, const char *redirect_url, const char *err_msg) {
    dyn_buf_t b;
    dyn_buf_init(&b, 1024);
    dyn_buf_append_str(&b, "<!DOCTYPE html>\n<html><head><meta charset='utf-8'>");
    dyn_buf_append_str(&b, "<title>Operation Failed</title>\n"
                           "<style>\n"
                           "body { font-family: monospace; background: #181818; color: #eee; padding: 24px; text-align: center; }\n"
                           ".card { background: #222; border: 1px solid #f85149; border-radius: 8px; max-width: 520px; margin: 40px auto; padding: 28px; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }\n"
                           "a { color: #58a6ff; text-decoration: none; font-weight: bold; }\n"
                           "a:hover { text-decoration: underline; }\n"
                           "</style></head><body>\n");
    dyn_buf_append_str(&b, "<div class='card'>\n");
    dyn_buf_append_str(&b, "<h2 style='color:#f85149;margin-top:0;'>❌ Operation Failed</h2>\n");
    char msg[512];
    snprintf(msg, sizeof(msg),
             "<p>%s</p>\n"
             "<p style='margin-top:20px;'><a href='%s'>⬅ Return to Directory</a></p>\n",
             err_msg ? err_msg : "An error occurred while saving the file.", redirect_url);
    dyn_buf_append_str(&b, msg);
    dyn_buf_append_str(&b, "</div></body></html>\n");

    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 400 Bad Request\r\n"
             "Content-Type: text/html; charset=UTF-8\r\n"
             "Content-Length: %u\r\n"
             "Server: IPO_OS/1.0\r\n"
             "Connection: close\r\n\r\n",
             (unsigned int)b.size);
    send(client_fd, hdr, strlen(hdr), 0);
    send(client_fd, b.data, b.size, 0);
    dyn_buf_free(&b);
}

/* Handle streaming file uploads (multipart/form-data or raw PUT/POST) directly to disk */
static void handle_file_upload_stream(int client_fd, const char *url_path, const char *fs_path,
                                      const char *content_type, long content_len,
                                      const uint8_t *initial_body, size_t initial_body_len) {
    char target_dir[256];
    struct ipo_inode dest_st;
    memset(&dest_st, 0, sizeof(dest_st));
    bool is_dir = false;

    if (strcmp(fs_path, "/") == 0) {
        is_dir = true;
        strncpy(target_dir, "/", sizeof(target_dir));
    } else if (ipo_stat(fs_path, &dest_st) == 0 && (dest_st.mode & IPO_INODE_TYPE_DIR)) {
        is_dir = true;
        strncpy(target_dir, fs_path, sizeof(target_dir));
    } else {
        get_parent_url(fs_path, target_dir, sizeof(target_dir));
    }

    const char *bpos = my_strcasestr(content_type, "boundary=");
    if (bpos) {
        /* Multipart upload from web browser form */
        bpos += 9;
        if (*bpos == '\"' || *bpos == '\'') bpos++;
        char boundary[128];
        size_t bi = 0;
        while (*bpos && *bpos != '\"' && *bpos != '\'' && *bpos != ';' &&
               *bpos != ' ' && *bpos != '\r' && *bpos != '\n' && bi + 1 < sizeof(boundary)) {
            boundary[bi++] = *bpos++;
        }
        boundary[bi] = '\0';

        char delim[160];
        snprintf(delim, sizeof(delim), "--%s", boundary);
        size_t delim_len = strlen(delim);

        /* Buffer for reading multipart header */
        uint8_t mp_buf[4096];
        size_t mp_buf_len = 0;

        if (initial_body && initial_body_len > 0) {
            size_t cp = (initial_body_len < sizeof(mp_buf)) ? initial_body_len : sizeof(mp_buf);
            memcpy(mp_buf, initial_body, cp);
            mp_buf_len = cp;
        }

        /* Read more until we have the part header (terminated by \r\n\r\n or \n\n) */
        uint32_t start_t = timer_millis();
        const uint8_t *hdr_end = NULL;
        size_t hdr_sep = 4;

        while (timer_millis() - start_t < 8000) {
            if (check_user_interrupted()) return;
            net_poll();

            hdr_end = mem_find(mp_buf, mp_buf_len, (const uint8_t *)"\r\n\r\n", 4);
            if (hdr_end) {
                hdr_sep = 4;
                break;
            }
            hdr_end = mem_find(mp_buf, mp_buf_len, (const uint8_t *)"\n\n", 2);
            if (hdr_end) {
                hdr_sep = 2;
                break;
            }

            if (mp_buf_len >= sizeof(mp_buf)) {
                break; /* Header too long */
            }

            ssize_t n = recv(client_fd, mp_buf + mp_buf_len, sizeof(mp_buf) - mp_buf_len, 0);
            if (n > 0) {
                mp_buf_len += (size_t)n;
            } else if (n == 0) {
                break;
            } else {
                ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
                io_wait();
            }
        }

        if (!hdr_end) {
            send_upload_error(client_fd, url_path, "Malformed multipart header or boundary missing");
            return;
        }

        /* Find boundary start */
        const uint8_t *part_start = mem_find(mp_buf, (size_t)(hdr_end - mp_buf), (const uint8_t *)delim, delim_len);
        if (!part_start) part_start = mp_buf;
        else part_start += delim_len;

        size_t part_hdr_len = (size_t)(hdr_end - part_start);
        char part_hdr[512];
        if (part_hdr_len >= sizeof(part_hdr)) part_hdr_len = sizeof(part_hdr) - 1;
        memcpy(part_hdr, part_start, part_hdr_len);
        part_hdr[part_hdr_len] = '\0';

        char filename[128] = "upload.bin";
        const char *fn = my_strcasestr(part_hdr, "filename=");
        if (fn) {
            fn += 9;
            while (*fn == ' ') fn++;
            char q = 0;
            if (*fn == '\"' || *fn == '\'') q = *fn++;
            size_t fi = 0;
            while (*fn && fi + 1 < sizeof(filename)) {
                if (q && *fn == q) break;
                if (!q && (*fn == ' ' || *fn == ';' || *fn == '\r' || *fn == '\n')) break;
                filename[fi++] = *fn++;
            }
            filename[fi] = '\0';
        }
        sanitize_filename(filename, sizeof(filename));

        /* Determine destination file path */
        char dest_path[256];
        if (strcmp(target_dir, "/") == 0) {
            snprintf(dest_path, sizeof(dest_path), "/%s", filename);
        } else {
            snprintf(dest_path, sizeof(dest_path), "%s/%s", target_dir, filename);
        }

        printf("[save] Multipart upload: streaming to '%s'...\n", dest_path);

        ipo_delete(dest_path);
        int c_res = ipo_create(dest_path, IPO_INODE_TYPE_FILE);
        if (c_res < 0) {
            send_upload_error(client_fd, url_path, "Failed to create destination file");
            return;
        }
        int fd = ipo_open(dest_path);
        if (fd < 0) {
            send_upload_error(client_fd, url_path, "Failed to open destination file for writing");
            return;
        }

        /* Move existing file data in mp_buf to start */
        const uint8_t *file_data = hdr_end + hdr_sep;
        size_t file_data_len = (size_t)((mp_buf + mp_buf_len) - file_data);
        if (file_data_len > 0) {
            memmove(mp_buf, file_data, file_data_len);
        }
        mp_buf_len = file_data_len;

        char end_delim[168];
        snprintf(end_delim, sizeof(end_delim), "\r\n--%s", boundary);
        size_t end_delim_len = strlen(end_delim);

        uint32_t total_written = 0;
        uint32_t last_activity = timer_millis();

        while (!check_user_interrupted()) {
            net_poll();

            /* Check if ending boundary is in mp_buf */
            const uint8_t *b_found = mem_find(mp_buf, mp_buf_len, (const uint8_t *)end_delim, end_delim_len);
            if (!b_found) {
                b_found = mem_find(mp_buf, mp_buf_len, (const uint8_t *)(end_delim + 1), end_delim_len - 1);
            }
            if (b_found) {
                size_t to_write = (size_t)(b_found - mp_buf);
                if (to_write > 0) {
                    ipo_write(fd, mp_buf, (uint32_t)to_write, total_written);
                    total_written += (uint32_t)to_write;
                }
                break;
            }

            /* No boundary yet: flush safe bytes to disk */
            size_t safe_tail = end_delim_len + 16;
            if (mp_buf_len > safe_tail) {
                size_t to_write = mp_buf_len - safe_tail;
                ipo_write(fd, mp_buf, (uint32_t)to_write, total_written);
                total_written += (uint32_t)to_write;
                memmove(mp_buf, mp_buf + to_write, safe_tail);
                mp_buf_len = safe_tail;
            }

            /* Read more data from socket */
            size_t space = sizeof(mp_buf) - mp_buf_len;
            ssize_t n = recv(client_fd, mp_buf + mp_buf_len, space, 0);
            if (n > 0) {
                mp_buf_len += (size_t)n;
                last_activity = timer_millis();
            } else if (n == 0) {
                if (mp_buf_len > 0) {
                    ipo_write(fd, mp_buf, (uint32_t)mp_buf_len, total_written);
                    total_written += (uint32_t)mp_buf_len;
                    mp_buf_len = 0;
                }
                break;
            } else {
                if (timer_millis() - last_activity > 8000) {
                    printf("[upload] Multipart read timeout (%u bytes written)\n", total_written);
                    break;
                }
                ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
                io_wait();
            }
        }

        ipo_close(fd);
        printf("[save] Multipart success! %u bytes written to '%s'\n", total_written, dest_path);
        send_upload_success(client_fd, url_path, filename, total_written);
    } else {
        /* Raw binary POST or PUT upload */
        char filename[128] = "upload.bin";
        char dest_path[256];

        if (is_dir) {
            snprintf(dest_path, sizeof(dest_path), "%s%s%s",
                     target_dir, (strcmp(target_dir, "/") == 0 ? "" : "/"), filename);
        } else {
            strncpy(dest_path, fs_path, sizeof(dest_path) - 1);
            dest_path[sizeof(dest_path) - 1] = '\0';
            const char *fn = get_filename(dest_path);
            if (fn && *fn) strncpy(filename, fn, sizeof(filename) - 1);
        }

        printf("[save] Raw upload: streaming to '%s' (content-length: %ld)...\n", dest_path, content_len);

        ipo_delete(dest_path);
        int c_res = ipo_create(dest_path, IPO_INODE_TYPE_FILE);
        if (c_res < 0) {
            send_upload_error(client_fd, url_path, "Failed to create destination file");
            return;
        }
        int fd = ipo_open(dest_path);
        if (fd < 0) {
            send_upload_error(client_fd, url_path, "Failed to open destination file for writing");
            return;
        }

        uint32_t total_written = 0;

        /* Write initial body bytes already read with headers */
        if (initial_body && initial_body_len > 0) {
            size_t to_write = initial_body_len;
            if (content_len >= 0 && (long)to_write > content_len) {
                to_write = (size_t)content_len;
            }
            ipo_write(fd, initial_body, (uint32_t)to_write, 0);
            total_written += (uint32_t)to_write;
        }

        char temp_chunk[2048];
        uint32_t last_activity = timer_millis();

        while (!check_user_interrupted()) {
            if (content_len >= 0 && (long)total_written >= content_len) {
                break;
            }
            size_t to_recv = sizeof(temp_chunk);
            if (content_len >= 0) {
                size_t rem = (size_t)content_len - total_written;
                if (rem < to_recv) to_recv = rem;
            }
            if (to_recv == 0) break;

            net_poll();
            ssize_t n = recv(client_fd, temp_chunk, to_recv, 0);
            if (n > 0) {
                ipo_write(fd, temp_chunk, (uint32_t)n, total_written);
                total_written += (uint32_t)n;
                last_activity = timer_millis();
            } else if (n == 0) {
                break;
            } else {
                if (timer_millis() - last_activity > 8000) {
                    printf("[upload] Raw upload timeout (%u bytes written)\n", total_written);
                    break;
                }
                ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
                io_wait();
            }
        }

        ipo_close(fd);
        printf("[save] Raw upload success! %u bytes written to '%s'\n", total_written, dest_path);
        send_upload_success(client_fd, url_path, filename, total_written);
    }
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
                              ".del { color: #f85149; font-size: 12px; margin-left: 12px; text-decoration: none !important; cursor: pointer; }\n"
                              ".del:hover { text-decoration: underline !important; }\n"
                              "button:hover { background: #2ea043 !important; }\n"
                              "input[type=file] { cursor: pointer; }\n"
                              "</style></head><body>\n");

    dyn_buf_append_str(&body, "<div class='info'>\n");
    dyn_buf_append_str(&body, "<h1 style='margin-top:0; color:#58a6ff;'>🚀 Hello from IPO_OS TCP/IP Stack!</h1>\n");
    dyn_buf_append_str(&body, "<p><b>OS Kernel:</b> IPO_OS</p>\n");
    dyn_buf_append_str(&body, "<p><b>Stack:</b> Custom RFC 793 / RFC 9293 TCP/IP stack</p>\n");

    char info[256];
    snprintf(info, sizeof(info), "<p><b>Your Client Address:</b> %s:%u</p>\n", client_ip_str, client_port);
    dyn_buf_append_str(&body, info);

    snprintf(info, sizeof(info), "<p><b>Total Requests Served:</b> %u &nbsp;|&nbsp; <b>Kernel Uptime:</b> %u s</p>\n",
             request_counter, timer_seconds());
    dyn_buf_append_str(&body, info);
    dyn_buf_append_str(&body, "</div>\n");

    /* 2. File Upload Form for current directory */
    char upload_box[1024];
    snprintf(upload_box, sizeof(upload_box),
             "<div style='background:#1e1e1e;border:1px solid #30363d;border-radius:6px;padding:16px;margin-bottom:20px;'>\n"
             "<h3 style='margin-top:0;margin-bottom:12px;color:#58a6ff;'>📤 Upload File to %s</h3>\n"
             "<form method='POST' action='%s' enctype='multipart/form-data' style='display:flex;align-items:center;gap:12px;flex-wrap:wrap;'>\n"
             "<input type='file' name='file' required style='background:#161b22;border:1px solid #30363d;padding:6px 10px;border-radius:4px;color:#c9d1d9;font-family:monospace;'>\n"
             "<button type='submit' style='background:#238636;color:#ffffff;border:none;padding:8px 16px;border-radius:6px;cursor:pointer;font-weight:bold;font-family:monospace;'>⬆ Upload File</button>\n"
             "</form>\n"
             "</div>\n",
             url_path, url_path);
    dyn_buf_append_str(&body, upload_box);

    /* 3. Directory listing section */
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

                char item_buf[600];
                if (is_dir) {
                    snprintf(item_buf, sizeof(item_buf),
                             "<li><span style='margin-right:8px;'>📁</span>"
                             "<a style='font-weight:bold;' href='%s'>%s/</a>"
                             "<span class='tag'>[DIR]</span>"
                             "<a class='del' href='javascript:void(0)' onclick=\"if(confirm('Delete directory %s?')){fetch('%s',{method:'DELETE'}).then(()=>location.reload());}\">[delete]</a></li>\n",
                             child_url, entry, entry, child_url);
                } else {
                    uint32_t sz = (uint32_t)c_st.size;
                    snprintf(item_buf, sizeof(item_buf),
                             "<li><span style='margin-right:8px;'>📄</span>"
                             "<a href='%s'>%s</a>"
                             "<span class='tag'>(%u bytes)</span>"
                             "<a class='down' href='%s' download>[download]</a>"
                             "<a class='del' href='javascript:void(0)' onclick=\"if(confirm('Delete file %s?')){fetch('%s',{method:'DELETE'}).then(()=>location.reload());}\">[delete]</a></li>\n",
                             child_url, entry, sz, child_url, entry, child_url);
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
    printf("   IPO_OS Full-Featured HTTP Server (RFC 7231)\n");
    printf("============================================================\n");
    printf("Device IP   : %s (link %s)\n", ip_str, (netif && netif->link_up) ? "UP" : "DOWN");
    printf("Listening on: 0.0.0.0:%u (INADDR_ANY)\n", port);
    printf("Methods     : GET, HEAD, POST, PUT, DELETE, OPTIONS, PATCH, TRACE\n\n");
    printf("Open in any web browser or HTTP client:\n");
    printf("  Root directory : http://%s:%u/\n", ip_str, port);
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

        /* 5. Read client request header */
        dyn_buf_t req_buf;
        dyn_buf_init(&req_buf, 1024);

        char temp_chunk[1024];
        ssize_t bytes_read;
        uint32_t read_start = timer_millis();

        while (timer_millis() - read_start < 4000) {
            if (check_user_interrupted()) {
                break;
            }
            net_poll();
            bytes_read = recv(client_fd, temp_chunk, sizeof(temp_chunk) - 1, 0);
            if (bytes_read > 0) {
                temp_chunk[bytes_read] = '\0';
                dyn_buf_append(&req_buf, temp_chunk, (size_t)bytes_read);

                if (strstr((const char *)req_buf.data, "\r\n\r\n") != NULL ||
                    strstr((const char *)req_buf.data, "\n\n") != NULL) {
                    break;
                }
            } else if (bytes_read <= 0) {
                break;
            }
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
            io_wait();
        }

        /* Determine header length and any already-read initial body bytes */
        const char *hdr_term = strstr((const char *)req_buf.data, "\r\n\r\n");
        size_t hdr_sep_len = 4;
        if (!hdr_term) {
            hdr_term = strstr((const char *)req_buf.data, "\n\n");
            hdr_sep_len = 2;
        }

        size_t header_len = 0;
        size_t initial_body_bytes = 0;
        const uint8_t *initial_body = NULL;
        if (hdr_term) {
            header_len = (size_t)(hdr_term - (const char *)req_buf.data) + hdr_sep_len;
            if (req_buf.size > header_len) {
                initial_body_bytes = req_buf.size - header_len;
                initial_body = req_buf.data + header_len;
            }
        } else {
            header_len = req_buf.size;
        }

        char method[16], url_path[256], query[256];
        parse_http_request((const char *)req_buf.data, method, sizeof(method),
                           url_path, sizeof(url_path), query, sizeof(query));
        printf("[http] %s %s%s%s\n", method, url_path, (query[0] ? "?" : ""), query);

        /* 6. Map URL path to IPO_FS filesystem path */
        char fs_path[256];
        map_url_to_fspath(url_path, fs_path, sizeof(fs_path));

        char content_type[256];
        parse_content_type((const char *)req_buf.data, header_len, content_type, sizeof(content_type));

        long content_len = parse_content_length((const char *)req_buf.data, header_len);

        /* 7. Dispatch HTTP Method */
        if (strcmp(method, "GET") == 0) {
            struct ipo_inode st;
            memset(&st, 0, sizeof(st));
            int stat_res = ipo_stat(fs_path, &st);

            if (stat_res == 0 && (st.mode & IPO_INODE_TYPE_DIR)) {
                send_directory(client_fd, url_path, fs_path, client_ip_str, client_port, request_counter);
            } else if (stat_res == 0 && (st.mode & IPO_INODE_TYPE_FILE)) {
                send_file(client_fd, fs_path, &st);
            } else {
                if (strcmp(fs_path, "/") == 0) {
                    send_directory(client_fd, url_path, "/", client_ip_str, client_port, request_counter);
                } else {
                    send_404(client_fd, url_path);
                }
            }
        } else if (strcmp(method, "HEAD") == 0) {
            send_head(client_fd, url_path, fs_path);
        } else if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
            handle_file_upload_stream(client_fd, url_path, fs_path, content_type,
                                      content_len, initial_body, initial_body_bytes);
        } else if (strcmp(method, "DELETE") == 0) {
            handle_delete(client_fd, url_path, fs_path);
        } else if (strcmp(method, "OPTIONS") == 0) {
            handle_options(client_fd, url_path);
        } else if (strcmp(method, "PATCH") == 0) {
            handle_patch_stream(client_fd, url_path, fs_path, query, content_len,
                                initial_body, initial_body_bytes);
        } else if (strcmp(method, "TRACE") == 0) {
            handle_trace(client_fd, &req_buf);
        } else {
            send_method_not_allowed(client_fd, method);
        }

        /* 8. Clean up and close connection */
        dyn_buf_free(&req_buf);
        close(client_fd);
    }

    printf("\n[server] Shutting down server socket...\n");
    close(server_fd);
    keyboard_set_app_input_mode(false);
    system_clear_interrupt();
    return 0;
}
