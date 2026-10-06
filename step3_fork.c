/*
 * step3_fork.c — Concurrent HTTP server with fork() and signal handling
 *
 * New features over step2:
 * - fork() for handling multiple clients simultaneously
 * - SIGCHLD handling to prevent zombie processes
 * - SIGINT/SIGTERM handling for graceful shutdown
 * - Logging with timestamps
 * - HEAD method support
 *
 * Build:  gcc -Wall -Wextra -o step3 step3_fork.c
 * Run:    ./step3
 * Test:   curl http://localhost:8080/ &
 *         curl http://localhost:8080/about.html &  (both run simultaneously!)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/wait.h>   /* waitpid() — for cleaning up child processes */
#include <fcntl.h>
#include <signal.h>     /* signal handling */
#include <time.h>       /* time formatting for logs and Date header */
#include <errno.h>      /* errno for error checking */

#define PORT 8080
#define BUFFER_SIZE 8192
#define WEB_ROOT "./www"

/*
 * Global variable for the server socket.
 * We need this in the signal handler to close it on shutdown.
 *
 * Using 'volatile sig_atomic_t' for the running flag because it's
 * modified by a signal handler and read by the main loop. This type
 * is guaranteed to be atomic — no partial reads/writes.
 */
static volatile sig_atomic_t g_running = 1;
static int g_server_fd = -1;

/* ─── Signal handlers ──────────────────────────────────────────────
 *
 * SIGCHLD: Sent to the parent when a child process exits.
 *   Without handling this, dead child processes become "zombies" —
 *   they show up in 'ps' and consume a process table slot.
 *   waitpid() with WNOHANG cleans them up without blocking.
 *
 * SIGINT/SIGTERM: Ctrl+C or kill command.
 *   We set g_running = 0 so the main loop exits cleanly.
 *   We also close the server socket to unblock accept().
 *
 * NOTE: Signal handlers should do as little as possible. Only call
 * "async-signal-safe" functions (write, _exit, etc.). printf is NOT
 * safe here, so we use write() for any output.
 */
void handle_sigchld(int sig)
{
    (void)sig;  /* unused parameter */

    /*
     * WNOHANG: don't block if no child has exited yet.
     * Loop: multiple children might have exited simultaneously.
     * We call waitpid() until there are no more dead children.
     */
    int saved_errno = errno;  /* save errno — signal handlers can change it */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;  /* keep collecting zombie processes */
    errno = saved_errno;  /* restore errno */
}

void handle_shutdown(int sig)
{
    (void)sig;
    const char msg[] = "\n[*] Shutting down...\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    g_running = 0;

    /* Close the server socket to unblock accept() */
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}

/* ─── Logging with timestamps ──────────────────────────────────────
 *
 * Real servers always log with timestamps. This helps when you're
 * debugging issues hours or days later — you need to know WHEN
 * something happened, not just WHAT happened.
 */
void log_request(const char *client_ip, const char *method,
                 const char *path, int status_code)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    printf("[%s] %s - %s %s → %d\n", time_buf, client_ip, method, path, status_code);
}

/* ─── Get HTTP-formatted date string ───────────────────────────────
 *
 * HTTP requires dates in a specific format:
 *   Wed, 01 Apr 2026 12:00:00 GMT
 *
 * This is defined in RFC 7231. The Date header is required in all
 * HTTP responses from a server.
 */
void get_http_date(char *buf, size_t buf_size)
{
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    strftime(buf, buf_size, "%a, %d %b %Y %H:%M:%S GMT", gmt);
}

/* ─── MIME types (same as step2) ──────────────────────────────── */
typedef struct { const char *ext; const char *mime; } MimeEntry;
static const MimeEntry MIME_TABLE[] = {
    { ".html", "text/html" },         { ".htm",  "text/html" },
    { ".css",  "text/css" },          { ".js",   "application/javascript" },
    { ".json", "application/json" },  { ".png",  "image/png" },
    { ".jpg",  "image/jpeg" },        { ".jpeg", "image/jpeg" },
    { ".gif",  "image/gif" },         { ".svg",  "image/svg+xml" },
    { ".ico",  "image/x-icon" },      { ".txt",  "text/plain" },
    { ".pdf",  "application/pdf" },   { ".xml",  "application/xml" },
    { ".mp4",  "video/mp4" },         { ".webm", "video/webm" },
    { ".woff2","font/woff2" },        { ".woff", "font/woff" },
    { NULL, NULL }
};

const char *get_mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (int i = 0; MIME_TABLE[i].ext; i++)
        if (strcasecmp(dot, MIME_TABLE[i].ext) == 0)
            return MIME_TABLE[i].mime;
    return "application/octet-stream";
}

/* ─── Send error response ─────────────────────────────────────── */
void send_error(int fd, int code, const char *text, int head_only)
{
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<html><body><h1>%d %s</h1>"
        "<hr><small>mini-httpd/1.0 (C)</small>"
        "</body></html>", code, text);

    char date[64];
    get_http_date(date, sizeof(date));

    char resp[1024];
    int resp_len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %d %s\r\n"
        "Date: %s\r\n"
        "Server: mini-httpd/1.0\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, text, date, body_len);

    write(fd, resp, resp_len);
    if (!head_only)
        write(fd, body, body_len);
}

/* ─── Send file response ──────────────────────────────────────── */
int send_file(int client_fd, const char *filepath, int head_only)
{
    struct stat st;
    if (stat(filepath, &st) < 0)
        return 404;
    if (!S_ISREG(st.st_mode))
        return 403;

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0)
        return 500;

    const char *mime = get_mime_type(filepath);
    char date[64];
    get_http_date(date, sizeof(date));

    /* Send headers */
    char headers[512];
    int hdr_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Date: %s\r\n"
        "Server: mini-httpd/1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        date, mime, (long)st.st_size);

    write(client_fd, headers, hdr_len);

    /*
     * HEAD requests: send only headers, no body.
     * Browsers use HEAD to check if a resource exists or has changed
     * without downloading the whole thing.
     */
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

/* ─── Parse & sanitize ────────────────────────────────────────── */
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

/* ─── Handle one client (runs in child process) ───────────────── */
void handle_client(int client_fd, struct sockaddr_in *client_addr)
{
    char *client_ip = inet_ntoa(client_addr->sin_addr);

    /* Read request */
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0)
        return;
    buffer[bytes_read] = '\0';

    /* Parse */
    char method[16], path[1024], version[16];
    if (parse_request(buffer, method, path, version) < 0) {
        send_error(client_fd, 400, "Bad Request", 0);
        log_request(client_ip, "???", "???", 400);
        return;
    }

    /* Only GET and HEAD */
    int head_only = (strcmp(method, "HEAD") == 0);
    if (strcmp(method, "GET") != 0 && !head_only) {
        send_error(client_fd, 405, "Method Not Allowed", 0);
        log_request(client_ip, method, path, 405);
        return;
    }

    /* Security check */
    if (!is_path_safe(path)) {
        send_error(client_fd, 400, "Bad Request", 0);
        log_request(client_ip, method, path, 400);
        return;
    }

    /* Build file path */
    char filepath[2048];
    if (strcmp(path, "/") == 0)
        snprintf(filepath, sizeof(filepath), "%s/index.html", WEB_ROOT);
    else
        snprintf(filepath, sizeof(filepath), "%s%s", WEB_ROOT, path);

    /* Serve */
    int status = send_file(client_fd, filepath, head_only);
    if (status == 404)
        send_error(client_fd, 404, "Not Found", head_only);
    else if (status == 403)
        send_error(client_fd, 403, "Forbidden", head_only);
    else if (status == 500)
        send_error(client_fd, 500, "Internal Server Error", head_only);

    log_request(client_ip, method, path, status);
}

/* ─── Main ────────────────────────────────────────────────────── */
int main(void)
{
    /*
     * Set up signal handlers BEFORE creating the socket.
     *
     * We use sigaction() instead of signal() because:
     * - sigaction() is POSIX-standard and portable
     * - signal() behavior varies between systems
     * - sigaction() lets us set flags like SA_RESTART
     *
     * SA_RESTART: If a signal interrupts a system call (like accept()),
     * automatically restart it instead of returning an error. Without
     * this, accept() would return EINTR every time a child exits.
     *
     * SA_NOCLDSTOP: Only notify us when children EXIT, not when they
     * stop (e.g., due to SIGSTOP). We don't care about stopped children.
     */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_shutdown;
    memset(&sa_shutdown, 0, sizeof(sa_shutdown));
    sa_shutdown.sa_handler = handle_shutdown;
    sigaction(SIGINT, &sa_shutdown, NULL);   /* Ctrl+C */
    sigaction(SIGTERM, &sa_shutdown, NULL);  /* kill command */

    /* Create socket */
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind() failed");
        close(g_server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(g_server_fd, 128) < 0) {
        perror("listen() failed");
        close(g_server_fd);
        exit(EXIT_FAILURE);
    }

    printf("[*] mini-httpd/1.0 started\n");
    printf("[*] Serving %s on http://localhost:%d\n", WEB_ROOT, PORT);
    printf("[*] PID: %d\n", getpid());
    printf("[*] Press Ctrl+C to stop\n\n");

    /*
     * Accept loop with fork()
     *
     * For each incoming connection:
     * 1. accept() returns a new socket for this client
     * 2. fork() creates a child process
     * 3. Child handles the request and exits
     * 4. Parent closes ITS copy of the client socket and goes back to accepting
     *
     * Why fork()? Each child is an independent process with its own memory.
     * If the child crashes (segfault, bad pointer), the parent keeps running.
     * This is how early Apache worked. Nginx uses a different model (event loop),
     * but fork() is simpler to understand and implement.
     *
     *   Parent process:            Child process:
     *   ┌─────────────┐            ┌─────────────┐
     *   │ accept()     │──fork()──→│ handle_client│
     *   │ close client │            │ close server │
     *   │ loop back    │            │ close client │
     *   └─────────────┘            │ exit         │
     *                              └─────────────┘
     */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (!g_running)
                break;  /* shutdown signal received */
            if (errno == EINTR)
                continue;  /* interrupted by signal, try again */
            perror("accept() failed");
            continue;
        }

        /*
         * fork() returns:
         *   - In the parent: the child's PID (positive number)
         *   - In the child: 0
         *   - On error: -1
         */
        pid_t pid = fork();

        if (pid < 0) {
            /* Fork failed — handle the client in the parent (fallback) */
            perror("fork() failed");
            handle_client(client_fd, &client_addr);
            close(client_fd);
        }
        else if (pid == 0) {
            /*
             * CHILD PROCESS
             *
             * Close the server socket — the child doesn't need it.
             * Handle the request, then exit.
             *
             * _exit() instead of exit():
             * - exit() flushes stdio buffers, runs atexit handlers
             * - _exit() exits immediately without cleanup
             * We use _exit() in the child to avoid flushing the parent's
             * buffered output twice.
             */
            close(g_server_fd);
            handle_client(client_fd, &client_addr);
            close(client_fd);
            _exit(0);
        }
        else {
            /*
             * PARENT PROCESS
             *
             * Close the client socket — the parent doesn't need it.
             * The child has its own copy (fork duplicates file descriptors).
             *
             * If we don't close this, we leak file descriptors. After
             * thousands of connections, the parent would run out of FDs
             * and accept() would fail with "Too many open files."
             *
             * You can verify with: lsof -p <parent_pid> | wc -l
             */
            close(client_fd);
        }
    }

    printf("[*] Server stopped. Goodbye!\n");
    return 0;
}
