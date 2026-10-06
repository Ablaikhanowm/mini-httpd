# mini-httpd

A static file HTTP/1.1 server written from scratch in C. No frameworks, no libraries beyond libc and POSIX.

Built as a learning project to understand how HTTP servers and CDN edge servers work at the protocol level.

## Features

- **Static file serving** from a configurable web root directory
- **Concurrent connections** via `fork()` with proper zombie prevention (`SIGCHLD`)
- **HTTP caching** — the core feature:
  - `ETag` generation based on inode, size, and modification time
  - `If-None-Match` → `304 Not Modified` (saves bandwidth)
  - `Last-Modified` / `If-Modified-Since` → `304 Not Modified`
  - `Cache-Control: public, max-age=N` with per-MIME-type TTLs
- **MIME type detection** for 17+ file types
- **Security**: directory traversal prevention (`..` blocking)
- **Graceful shutdown** on `SIGINT` / `SIGTERM`
- **HEAD method** support
- **Timestamped request logging**

## Build & Run

```bash
make            # debug build (with -g for gdb)
make release    # optimized build
make run        # build + create sample files + start server
make test       # automated test of caching behavior
make valgrind   # memory leak check
make clean      # remove binaries
```

## Usage

```bash
# Start the server
./httpd

# In another terminal:
curl -v http://localhost:8080/              # serves www/index.html
curl -v http://localhost:8080/style.css     # serves with text/css Content-Type

# Test caching — first request returns 200 with ETag:
curl -v http://localhost:8080/index.html
# Response includes: ETag: "1a2b3c-5a-662fa1b0"

# Second request with ETag — returns 304 (no body transferred!):
curl -v -H 'If-None-Match: "1a2b3c-5a-662fa1b0"' http://localhost:8080/index.html
# Response: HTTP/1.1 304 Not Modified
```

## How Caching Works

This server implements the same caching semantics used by CDN edge servers:

```
Client                          Server
  |                               |
  |--- GET /logo.png ----------->|
  |                               |
  |<-- 200 OK -------------------|  (full file + ETag + Last-Modified + Cache-Control)
  |    ETag: "abc123"             |
  |    Cache-Control: max-age=604800
  |                               |
  |   ... time passes ...         |
  |                               |
  |--- GET /logo.png ----------->|
  |    If-None-Match: "abc123"    |
  |                               |
  |<-- 304 Not Modified ---------|  (no body! client uses cached version)
  |                               |
```

Cache-Control max-age varies by file type:
| Type | max-age | Duration |
|------|---------|----------|
| HTML | 300 | 5 minutes |
| CSS/JS | 86400 | 1 day |
| Images | 604800 | 1 week |
| Fonts | 2592000 | 30 days |

## Project Structure

```
.
├── httpd.c          # Complete server (final version)
├── step1_hello.c    # Step 1: bare minimum socket server
├── step2_files.c    # Step 2: + request parsing + file serving
├── step3_fork.c     # Step 3: + fork() + signal handling
├── Makefile
├── README.md
└── www/             # Web root (served files go here)
    ├── index.html
    ├── about.html
    ├── style.css
    └── data.json
```

The `step*.c` files show the progression from a 50-line echo server to the full implementation. Each step adds one major feature.

## What I Learned

- POSIX socket programming: `socket()`, `bind()`, `listen()`, `accept()`, `read()`, `write()`
- HTTP/1.1 protocol: request parsing, status codes, headers, caching semantics
- Process management: `fork()`, `waitpid()`, zombie prevention, signal handling
- Memory safety: no leaks verified with Valgrind, buffer overflow prevention
- File I/O: `open()`, `read()`, `stat()`, chunked file transfer
- Security: path sanitization against directory traversal attacks

## Technologies

C (C11), POSIX sockets, Linux system calls

## License

MIT
