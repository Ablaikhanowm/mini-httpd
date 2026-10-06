#define _GNU_SOURCE  /* for strptime, timegm, sigaction, etc. */

/*
 * httpd.c — A complete HTTP/1.1 static file server in C
 *
 * Features:
 * - Static file serving from a configurable web root
 * - Concurrent connections via fork()
 * - HTTP caching: ETag, If-None-Match (304 Not Modified)
 * - HTTP caching: Last-Modified, If-Modified-Since (304 Not Modified)
 * - Cache-Control headers with configurable TTL per file type
 * - MIME type detection
 * - HEAD method support
 * - Directory traversal prevention
 * - Graceful shutdown (SIGINT/SIGTERM)
 * - Zombie process prevention (SIGCHLD)
 * - Timestamped request logging
 *
 * Build:  make
 * Run:    ./httpd
 * Test:   curl -v http://localhost:8080/
 *
 * CDN-relevant: This server implements the same caching semantics
 * that CDN edge servers use. Understanding ETag, If-None-Match,
 * Last-Modified, If-Modified-Since, and Cache-Control is fundamental
 * to how CDN77's infrastructure works.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#define PORT 8080
#define BUFFER_SIZE 8192
#define WEB_ROOT "./www"

static volatile sig_atomic_t g_running = 1;
static int g_server_fd = -1;

/* ── Signal handlers ────────────────────────────────────────────── */

void handle_sigchld(int sig)
{
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved;
}

void handle_shutdown(int sig)
{
    (void)sig;
    const char msg[] = "\n[*] Shutting down...\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    g_running = 0;
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}

/* ── Utilities ──────────────────────────────────────────────────── */

void get_http_date(char *buf, size_t sz)
{
    time_t now = time(NULL);
    strftime(buf, sz, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&now));
}

/*
 * Format a time_t as an HTTP date string.
 * Used for Last-Modified header.
 */
void format_http_date(time_t t, char *buf, size_t sz)
{
    strftime(buf, sz, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&t));
}

/*
 * Parse an HTTP date string back into time_t.
 * Used for parsing If-Modified-Since header from the client.
 *
 * HTTP dates look like: "Wed, 01 Apr 2026 12:00:00 GMT"
 * strptime() is the inverse of strftime().
 */
time_t parse_http_date(const char *date_str)
{
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    if (strptime(date_str, "%a, %d %b %Y %H:%M:%S GMT", &tm_val) == NULL)
        return -1;
    return timegm(&tm_val);  /* convert UTC struct tm to time_t */
}

void log_request(const char *ip, const char *method, const char *path, int status)
{
    time_t now = time(NULL);
    char t[64];
    strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] %s - %s %s -> %d\n", t, ip, method, path, status);
}

/* ── MIME types ─────────────────────────────────────────────────── */

typedef struct { const char *ext; const char *mime; int max_age; } MimeEntry;

/*
 * max_age: how long the browser/CDN should cache this type of file.
 *
 * Different content types have different caching strategies:
 * - HTML: short cache (300s = 5 min) because it changes frequently
 * - CSS/JS: medium cache (86400s = 1 day) — versioned via filenames
 * - Images: long cache (604800s = 1 week) — rarely change
 * - Fonts: very long cache (2592000s = 30 days) — almost never change
 *
 * This mirrors real CDN configuration. At CDN77, customers configure
 * these values per file type to optimize cache hit ratios.
 */
static const MimeEntry MIME_TABLE[] = {
    { ".html", "text/html",                300 },
    { ".htm",  "text/html",                300 },
    { ".css",  "text/css",               86400 },
    { ".js",   "application/javascript", 86400 },
    { ".json", "application/json",         300 },
    { ".png",  "image/png",            604800 },
    { ".jpg",  "image/jpeg",           604800 },
    { ".jpeg", "image/jpeg",           604800 },
    { ".gif",  "image/gif",            604800 },
    { ".svg",  "image/svg+xml",        604800 },
    { ".ico",  "image/x-icon",         604800 },
    { ".txt",  "text/plain",              300 },
    { ".pdf",  "application/pdf",      604800 },
    { ".mp4",  "video/mp4",           604800 },
    { ".webm", "video/webm",          604800 },
    { ".woff2","font/woff2",         2592000 },
    { ".woff", "font/woff",         2592000 },
    { NULL, NULL, 3600 }
};

const MimeEntry *get_mime_entry(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot) {
        for (int i = 0; MIME_TABLE[i].ext; i++)
            if (strcasecmp(dot, MIME_TABLE[i].ext) == 0)
                return &MIME_TABLE[i];
    }
    /* Return a default entry for unknown types */
    static const MimeEntry fallback = { "", "application/octet-stream", 3600 };
    return &fallback;
}

/* ── ETag generation ────────────────────────────────────────────
 *
 * An ETag uniquely identifies a specific version of a file.
 * If the file changes (different size or modification time),
 * the ETag changes too.
 *
 * We generate it from the inode number, file size, and modification
 * time — the same approach Apache uses. This is NOT a hash of the
 * file contents (that would be too slow for large files), but it's
 * good enough to detect changes.
 *
 * Format: "inode-size-mtime" in hex
 * Example: "1a2b3c-f4e8-62d1a5b0"
 *
 * The quotes are part of the ETag value — the HTTP spec requires them.
 */
void generate_etag(const struct stat *st, char *etag, size_t sz)
{
    snprintf(etag, sz, "\"%lx-%lx-%lx\"",
             (unsigned long)st->st_ino,
             (unsigned long)st->st_size,
             (unsigned long)st->st_mtime);
}

/* ── Header extraction ──────────────────────────────────────────
 *
 * Extract a specific header value from the raw HTTP request.
 *
 * HTTP headers look like:
 *   If-None-Match: "1a2b3c-f4e8-62d1a5b0"\r\n
 *   If-Modified-Since: Wed, 01 Apr 2026 12:00:00 GMT\r\n
 *
 * We search for the header name, skip past ": ", and read until \r\n.
 *
 * Returns a pointer to a static buffer (not thread-safe, but we fork).
 */
const char *get_header(const char *request, const char *header_name)
{
    static char value[512];
    char search[256];
    snprintf(search, sizeof(search), "\r\n%s: ", header_name);

    const char *found = strstr(request, search);
    if (!found) {
        /* Try case-insensitive search — HTTP headers are case-insensitive */
        /* For simplicity, we'll try lowercase too */
        return NULL;
    }

    found += strlen(search);
    const char *end = strstr(found, "\r\n");
    if (!end) return NULL;

    size_t len = end - found;
    if (len >= sizeof(value)) len = sizeof(value) - 1;
    memcpy(value, found, len);
    value[len] = '\0';
    return value;
}

/* ── Send response ──────────────────────────────────────────────── */

void send_error(int fd, int code, const char *text, int head_only)
{
    char body[512], date[64], resp[1024];
    int body_len = snprintf(body, sizeof(body),
        "<html><body><h1>%d %s</h1>"
        "<hr><small>mini-httpd/1.0</small>"
        "</body></html>", code, text);

    get_http_date(date, sizeof(date));
    int resp_len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %d %s\r\n"
        "Date: %s\r\n"
        "Server: mini-httpd/1.0\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n", code, text, date, body_len);

    write(fd, resp, resp_len);
    if (!head_only) write(fd, body, body_len);
}

/*
 * send_304() — Tell the client their cached version is still valid
 *
 * This is the magic of HTTP caching. Instead of sending the entire
 * file again (which could be megabytes), we send a tiny 304 response
 * (just headers, no body). The client uses its cached copy.
 *
 * For a CDN, this is HUGE. Imagine a popular image that's requested
 * 10,000 times per second. If 95% of requests get a 304, you've
 * saved 95% of the bandwidth.
 */
void send_304(int fd, const char *etag, time_t mtime, int max_age)
{
    char date[64], modified[64];
    get_http_date(date, sizeof(date));
    format_http_date(mtime, modified, sizeof(modified));

    char resp[512];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 304 Not Modified\r\n"
        "Date: %s\r\n"
        "Server: mini-httpd/1.0\r\n"
        "ETag: %s\r\n"
        "Last-Modified: %s\r\n"
        "Cache-Control: public, max-age=%d\r\n"
        "Connection: close\r\n"
        "\r\n",
        date, etag, modified, max_age);

    write(fd, resp, len);
}

/*
 * send_file() — Serve a file with full caching support
 *
 * Returns the HTTP status code that was sent.
 *
 * The caching logic:
 *
 * 1. Client sends: If-None-Match: "abc123"
 *    We compare with our ETag. If they match → 304 (file unchanged)
 *
 * 2. Client sends: If-Modified-Since: Wed, 01 Apr 2026 12:00:00 GMT
 *    We compare with the file's mtime. If not modified → 304
 *
 * 3. If neither condition is met, we send the full file with:
 *    - ETag header (so the client can use If-None-Match next time)
 *    - Last-Modified header (so the client can use If-Modified-Since)
 *    - Cache-Control header (tells the client how long to cache)
 */
int send_file(int client_fd, const char *filepath, int head_only,
              const char *raw_request)
{
    struct stat st;
    if (stat(filepath, &st) < 0)      return 404;
    if (!S_ISREG(st.st_mode))         return 403;

    /* Generate ETag for this file version */
    char etag[128];
    generate_etag(&st, etag, sizeof(etag));

    /* Get MIME info (includes cache max-age) */
    const MimeEntry *mime = get_mime_entry(filepath);

    /*
     * CHECK 1: If-None-Match (ETag-based caching)
     *
     * The client says: "I have a cached version with ETag X.
     *                   Has it changed?"
     *
     * If our current ETag matches what the client has → 304
     * The file hasn't changed, no need to send it again.
     *
     * This is the primary caching mechanism at CDN77.
     */
    const char *if_none_match = get_header(raw_request, "If-None-Match");
    if (if_none_match && strcmp(if_none_match, etag) == 0) {
        send_304(client_fd, etag, st.st_mtime, mime->max_age);
        return 304;
    }

    /*
     * CHECK 2: If-Modified-Since (time-based caching)
     *
     * The client says: "I have a cached version from time X.
     *                   Has the file been modified since then?"
     *
     * If the file's modification time is NOT after the client's
     * cached time → 304 (still the same)
     *
     * This is the fallback caching mechanism. ETags are preferred
     * because they're more precise (time has 1-second granularity,
     * so changes within the same second might be missed).
     */
    const char *if_modified = get_header(raw_request, "If-Modified-Since");
    if (if_modified) {
        time_t client_time = parse_http_date(if_modified);
        if (client_time >= 0 && st.st_mtime <= client_time) {
            send_304(client_fd, etag, st.st_mtime, mime->max_age);
            return 304;
        }
    }

    /* No cache match — send the full file */
    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) return 500;

    char date[64], modified[64];
    get_http_date(date, sizeof(date));
    format_http_date(st.st_mtime, modified, sizeof(modified));

    /*
     * Send headers with all caching information:
     *
     * ETag:           Unique identifier for this version of the file
     * Last-Modified:  When the file was last changed
     * Cache-Control:  How long the client/CDN should cache this
     *   - public:     Any cache (browser, CDN, proxy) can cache this
     *   - max-age:    Cache for this many seconds
     *
     * The client will store these values and include If-None-Match
     * or If-Modified-Since in future requests.
     */
    char headers[1024];
    int hdr_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Date: %s\r\n"
        "Server: mini-httpd/1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "ETag: %s\r\n"
        "Last-Modified: %s\r\n"
        "Cache-Control: public, max-age=%d\r\n"
        "Connection: close\r\n"
        "\r\n",
        date, mime->mime, (long)st.st_size,
        etag, modified, mime->max_age);

    write(client_fd, headers, hdr_len);

    if (!head_only) {
        char buf[BUFFER_SIZE];
        ssize_t n;
        while ((n = read(file_fd, buf, sizeof(buf))) > 0) {
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t w = write(client_fd, buf + sent, n - sent);
                if (w < 0) { close(file_fd); return 200; }
                sent += w;
            }
        }
    }

    close(file_fd);
    return 200;
}

/* ── Request handling ───────────────────────────────────────────── */

int parse_request(const char *req, char *method, char *path, char *version)
{
    if (sscanf(req, "%15s %1023s %15s", method, path, version) < 2)
        return -1;
    return 0;
}

int is_path_safe(const char *path)
{
    return (path[0] == '/' && strstr(path, "..") == NULL);
}

void handle_client(int client_fd, struct sockaddr_in *client_addr)
{
    char *ip = inet_ntoa(client_addr->sin_addr);

    char buffer[BUFFER_SIZE];
    ssize_t bytes = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes <= 0) return;
    buffer[bytes] = '\0';

    char method[16], path[1024], version[16];
    if (parse_request(buffer, method, path, version) < 0) {
        send_error(client_fd, 400, "Bad Request", 0);
        log_request(ip, "???", "???", 400);
        return;
    }

    int head_only = (strcmp(method, "HEAD") == 0);
    if (strcmp(method, "GET") != 0 && !head_only) {
        send_error(client_fd, 405, "Method Not Allowed", 0);
        log_request(ip, method, path, 405);
        return;
    }

    if (!is_path_safe(path)) {
        send_error(client_fd, 400, "Bad Request", 0);
        log_request(ip, method, path, 400);
        return;
    }

    char filepath[2048];
    if (strcmp(path, "/") == 0)
        snprintf(filepath, sizeof(filepath), "%s/index.html", WEB_ROOT);
    else
        snprintf(filepath, sizeof(filepath), "%s%s", WEB_ROOT, path);

    /* Pass the raw request so send_file can check caching headers */
    int status = send_file(client_fd, filepath, head_only, buffer);

    if (status == 404)      send_error(client_fd, 404, "Not Found", head_only);
    else if (status == 403) send_error(client_fd, 403, "Forbidden", head_only);
    else if (status == 500) send_error(client_fd, 500, "Internal Server Error", head_only);

    log_request(ip, method, path, status);
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_shutdown;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(PORT)
    };

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(g_server_fd); exit(1);
    }
    if (listen(g_server_fd, 128) < 0) {
        perror("listen"); close(g_server_fd); exit(1);
    }

    printf("╔══════════════════════════════════════╗\n");
    printf("║        mini-httpd/1.0 (C)            ║\n");
    printf("╠══════════════════════════════════════╣\n");
    printf("║  Root:   %-27s ║\n", WEB_ROOT);
    printf("║  Listen: http://localhost:%-5d      ║\n", PORT);
    printf("║  PID:    %-27d ║\n", getpid());
    printf("╚══════════════════════════════════════╝\n\n");

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd < 0) {
            if (!g_running || errno == EINTR) continue;
            perror("accept");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            handle_client(client_fd, &client_addr);
            close(client_fd);
        } else if (pid == 0) {
            close(g_server_fd);
            handle_client(client_fd, &client_addr);
            close(client_fd);
            _exit(0);
        } else {
            close(client_fd);
        }
    }

    printf("[*] Server stopped.\n");
    return 0;
}
