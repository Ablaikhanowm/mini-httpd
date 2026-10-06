/*
 * step2_files.c — HTTP server that parses requests and serves files
 *
 * New features over step1:
 * - Parses the HTTP request line (method, path, version)
 * - Serves files from a "www" directory
 * - Returns 404 for missing files
 * - Returns 400 for bad requests
 * - Returns 405 for non-GET methods
 * - Detects Content-Type based on file extension
 * - Serves /index.html when / is requested
 *
 * Build:  gcc -Wall -Wextra -o step2 step2_files.c
 * Run:    mkdir -p www && echo "<h1>Home</h1>" > www/index.html && ./step2
 * Test:   curl http://localhost:8080/
 *         curl http://localhost:8080/index.html
 *         curl http://localhost:8080/nonexistent.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>   /* stat() — to get file info */
#include <fcntl.h>      /* open() */

#define PORT 8080
#define BUFFER_SIZE 8192
#define WEB_ROOT "./www"

/* ─── MIME type detection ──────────────────────────────────────────
 *
 * When the server sends a file, it needs to tell the client what kind
 * of file it is. The Content-Type header does this. The client (browser)
 * uses it to decide how to display the content.
 *
 * At CDN77, getting Content-Type right is critical — if a CDN serves
 * a CSS file as text/plain, the website's styling breaks.
 */
typedef struct {
    const char *extension;
    const char *mime_type;
} MimeEntry;

static const MimeEntry MIME_TABLE[] = {
    { ".html", "text/html" },
    { ".htm",  "text/html" },
    { ".css",  "text/css" },
    { ".js",   "application/javascript" },
    { ".json", "application/json" },
    { ".png",  "image/png" },
    { ".jpg",  "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".gif",  "image/gif" },
    { ".svg",  "image/svg+xml" },
    { ".ico",  "image/x-icon" },
    { ".txt",  "text/plain" },
    { ".pdf",  "application/pdf" },
    { ".xml",  "application/xml" },
    { NULL,    NULL }  /* sentinel — marks the end of the table */
};

/*
 * get_mime_type() — Find the MIME type for a file path
 *
 * We look at the file extension and match it against our table.
 * If no match is found, we return "application/octet-stream" which
 * means "unknown binary data" — the safe default.
 *
 * strrchr() finds the LAST occurrence of a character in a string.
 * So for "images/photo.backup.jpg", it finds the last '.' and gives
 * us ".jpg" — the actual extension.
 *
 * strcasecmp() compares strings ignoring case, so ".JPG" matches ".jpg".
 */
const char *get_mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";

    for (int i = 0; MIME_TABLE[i].extension != NULL; i++) {
        if (strcasecmp(dot, MIME_TABLE[i].extension) == 0)
            return MIME_TABLE[i].mime_type;
    }
    return "application/octet-stream";
}

/* ─── Send an HTTP error response ──────────────────────────────────
 *
 * Helper function to send error responses (400, 404, 405, 500).
 * Builds a complete HTTP response with a simple HTML error page.
 */
void send_error(int client_fd, int status_code, const char *status_text)
{
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<html><body>"
        "<h1>%d %s</h1>"
        "<p>The requested resource could not be served.</p>"
        "<hr><small>mini-httpd/1.0 (C)</small>"
        "</body></html>",
        status_code, status_text);

    char response[1024];
    int response_len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status_code, status_text, body_len, body);

    write(client_fd, response, response_len);
}

/* ─── Send a file as HTTP response ─────────────────────────────────
 *
 * This reads a file from disk and sends it as an HTTP response.
 *
 * We use stat() to get the file size BEFORE reading it, so we can
 * set Content-Length correctly. Then we read the file in chunks and
 * send each chunk.
 *
 * Why chunks? Because the file might be larger than our buffer.
 * A 50MB video doesn't fit in a 8KB buffer, so we read 8KB at a time
 * and send each piece.
 */
void send_file(int client_fd, const char *filepath)
{
    /* stat() gets file metadata without opening it */
    struct stat file_stat;
    if (stat(filepath, &file_stat) < 0) {
        send_error(client_fd, 404, "Not Found");
        return;
    }

    /* Make sure it's a regular file, not a directory */
    if (!S_ISREG(file_stat.st_mode)) {
        send_error(client_fd, 403, "Forbidden");
        return;
    }

    /* Open the file for reading */
    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) {
        send_error(client_fd, 500, "Internal Server Error");
        return;
    }

    /* Send HTTP headers first */
    const char *mime = get_mime_type(filepath);
    char headers[512];
    int headers_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        mime, (long)file_stat.st_size);

    write(client_fd, headers, headers_len);

    /*
     * Send the file body in chunks.
     *
     * read() returns the number of bytes actually read.
     * It might return less than BUFFER_SIZE (especially for the last chunk).
     * When it returns 0, we've reached the end of the file.
     */
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(file_fd, buffer, sizeof(buffer))) > 0) {
        /*
         * Important: write() might not send all bytes in one call.
         * For robustness, we should loop until all bytes are sent.
         * This is called "handling partial writes."
         */
        ssize_t total_sent = 0;
        while (total_sent < bytes_read) {
            ssize_t sent = write(client_fd, buffer + total_sent,
                                 bytes_read - total_sent);
            if (sent < 0) {
                perror("write() failed");
                close(file_fd);
                return;
            }
            total_sent += sent;
        }
    }

    close(file_fd);
}

/* ─── Parse the HTTP request ───────────────────────────────────────
 *
 * An HTTP request line looks like:
 *   GET /images/logo.png HTTP/1.1
 *
 * We need to extract:
 *   method  = "GET"
 *   path    = "/images/logo.png"
 *   version = "HTTP/1.1"
 *
 * sscanf() with %s reads whitespace-delimited strings — perfect for
 * this format. We limit each field to prevent buffer overflows.
 *
 * Returns 0 on success, -1 on failure.
 */
int parse_request(const char *request, char *method, size_t method_sz,
                  char *path, size_t path_sz, char *version, size_t version_sz)
{
    /* %255s means "read at most 255 characters" — prevents overflow */
    int matched = sscanf(request, "%255s %1023s %31s", method, path, version);
    (void)method_sz;  /* suppress unused warnings — sizes are for documentation */
    (void)path_sz;
    (void)version_sz;

    if (matched < 2)  /* need at least method and path */
        return -1;

    return 0;
}

/* ─── Security: Sanitize the path ──────────────────────────────────
 *
 * CRITICAL SECURITY CHECK: prevent directory traversal attacks.
 *
 * If a client requests "GET /../../etc/passwd HTTP/1.1", we must NOT
 * serve /etc/passwd! The client is trying to escape the web root
 * directory using ".." to go up.
 *
 * We check if the path contains ".." — if it does, reject it.
 * A real server would do more thorough checking (resolve symlinks,
 * canonicalize the path), but this catches the common attacks.
 */
int is_path_safe(const char *path)
{
    if (strstr(path, "..") != NULL)
        return 0;  /* unsafe! */
    if (path[0] != '/')
        return 0;  /* path must start with / */
    return 1;  /* safe */
}

/* ─── Main ─────────────────────────────────────────────────────── */
int main(void)
{
    /* Create socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind() failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    /* Listen */
    if (listen(server_fd, 128) < 0) {
        perror("listen() failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("[*] Serving files from %s on http://localhost:%d\n", WEB_ROOT, PORT);
    printf("[*] Press Ctrl+C to stop\n\n");

    /* Accept loop */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept() failed");
            continue;
        }

        /* Read the request */
        char buffer[BUFFER_SIZE];
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read <= 0) {
            close(client_fd);
            continue;
        }
        buffer[bytes_read] = '\0';

        /* Parse the request line */
        char method[256];
        char path[1024];
        char version[32];

        if (parse_request(buffer, method, sizeof(method),
                          path, sizeof(path), version, sizeof(version)) < 0) {
            printf("[!] Bad request from %s\n", inet_ntoa(client_addr.sin_addr));
            send_error(client_fd, 400, "Bad Request");
            close(client_fd);
            continue;
        }

        printf("[%s] %s %s\n", inet_ntoa(client_addr.sin_addr), method, path);

        /* Only support GET */
        if (strcmp(method, "GET") != 0) {
            send_error(client_fd, 405, "Method Not Allowed");
            close(client_fd);
            continue;
        }

        /* Security check */
        if (!is_path_safe(path)) {
            printf("[!] Blocked directory traversal attempt: %s\n", path);
            send_error(client_fd, 400, "Bad Request");
            close(client_fd);
            continue;
        }

        /* Map URL path to file path */
        char filepath[2048];

        /* If path is "/" serve index.html */
        if (strcmp(path, "/") == 0) {
            snprintf(filepath, sizeof(filepath), "%s/index.html", WEB_ROOT);
        } else {
            snprintf(filepath, sizeof(filepath), "%s%s", WEB_ROOT, path);
        }

        /* Serve the file */
        send_file(client_fd, filepath);

        close(client_fd);
    }

    close(server_fd);
    return 0;
}
