/*
 * step1_hello.c — The simplest possible HTTP server
 *
 * This server:
 * - Listens on port 8080
 * - Accepts one connection at a time
 * - Sends "Hello World" as an HTTP response to every request
 * - Closes the connection and waits for the next one
 *
 * Build:  gcc -Wall -Wextra -o step1 step1_hello.c
 * Run:    ./step1
 * Test:   curl http://localhost:8080
 */

#include <stdio.h>      /* printf, perror */
#include <stdlib.h>     /* exit, EXIT_FAILURE */
#include <string.h>     /* memset */
#include <unistd.h>     /* close, read, write */
#include <sys/socket.h> /* socket, bind, listen, accept */
#include <netinet/in.h> /* struct sockaddr_in, INADDR_ANY */
#include <arpa/inet.h>  /* inet_ntoa */

#define PORT 8080
#define BUFFER_SIZE 4096

int main(void)
{
    /*
     * STEP 1: Create a socket
     *
     * socket() creates a communication endpoint and returns a file descriptor.
     *
     * AF_INET     = IPv4 internet protocols
     * SOCK_STREAM = TCP (reliable, connection-oriented)
     * 0           = let the OS choose the protocol (TCP for SOCK_STREAM)
     *
     * The returned value is a "file descriptor" — just an integer that
     * represents this socket. In Linux, everything is a file, including
     * network connections. You'll use this number for all future operations
     * on this socket.
     */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }
    printf("[*] Socket created (fd=%d)\n", server_fd);

    /*
     * STEP 1.5: Set SO_REUSEADDR
     *
     * Without this, if you stop and restart the server quickly, you'll get
     * "Address already in use" because the old socket is still in TIME_WAIT
     * state. SO_REUSEADDR tells the OS to let us bind to the port anyway.
     *
     * You'll encounter TIME_WAIT constantly when working at CDN77 —
     * it's a normal part of TCP connection teardown.
     */
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt() failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    /*
     * STEP 2: Bind the socket to an address and port
     *
     * We need to tell the OS: "I want this socket to listen on port 8080
     * on all network interfaces (0.0.0.0)."
     *
     * struct sockaddr_in is the IPv4 address structure:
     *   - sin_family: AF_INET (IPv4)
     *   - sin_addr.s_addr: which IP to bind to (INADDR_ANY = all interfaces)
     *   - sin_port: which port (must be in network byte order — htons())
     *
     * htons() = "host to network short" — converts the port number from
     * your computer's byte order to the network's byte order (big-endian).
     * Without this, the port number would be interpreted wrong on
     * little-endian machines (which is most modern CPUs).
     */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));  /* zero out the struct */
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;      /* bind to 0.0.0.0 */
    server_addr.sin_port        = htons(PORT);      /* bind to port 8080 */

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind() failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("[*] Bound to port %d\n", PORT);

    /*
     * STEP 3: Start listening
     *
     * listen() marks the socket as a passive socket — one that will be
     * used to accept incoming connections (as opposed to connecting out).
     *
     * The second argument (128) is the "backlog" — the maximum number of
     * pending connections waiting to be accepted. If more than 128 clients
     * try to connect before we call accept(), the extras get refused.
     *
     * At CDN77, nginx's listen backlog is tuned much higher for handling
     * thousands of simultaneous connections.
     */
    if (listen(server_fd, 128) < 0) {
        perror("listen() failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("[*] Listening on http://localhost:%d\n", PORT);
    printf("[*] Press Ctrl+C to stop\n\n");

    /*
     * STEP 4: Accept connections in a loop
     *
     * accept() blocks (waits) until a client connects. When one does,
     * it returns a NEW file descriptor for that specific connection.
     *
     * The original server_fd continues listening for more connections.
     * The new client_fd is used to talk to this specific client.
     *
     * Think of it like a restaurant: server_fd is the front door (always
     * open for new guests), and client_fd is the waiter assigned to
     * one specific table.
     */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept() failed");
            continue;  /* don't crash — just try accepting the next one */
        }

        /* Print who connected */
        printf("[+] Connection from %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        /*
         * STEP 5: Read the client's request
         *
         * The client (e.g., curl or a browser) sends an HTTP request.
         * We read it into a buffer. For now, we don't parse it — we just
         * print it so you can see what an HTTP request looks like.
         *
         * read() returns the number of bytes read, or -1 on error.
         * We null-terminate the buffer so we can print it as a string.
         */
        char buffer[BUFFER_SIZE];
        ssize_t bytes_read = read(client_fd, buffer, BUFFER_SIZE - 1);
        if (bytes_read < 0) {
            perror("read() failed");
            close(client_fd);
            continue;
        }
        buffer[bytes_read] = '\0';  /* null-terminate */

        /* Print the raw HTTP request so you can see it */
        printf("--- Request ---\n%s--- End ---\n\n", buffer);

        /*
         * STEP 6: Send an HTTP response
         *
         * An HTTP response has this structure:
         *
         *   HTTP/1.1 200 OK\r\n          ← status line
         *   Content-Type: text/html\r\n   ← header
         *   Content-Length: 44\r\n         ← header (length of body)
         *   Connection: close\r\n          ← header (we'll close after)
         *   \r\n                           ← empty line = end of headers
         *   <html><body>Hello World!</body></html>  ← body
         *
         * \r\n (CRLF) is required by the HTTP spec as the line terminator.
         * The empty line (\r\n\r\n) separates headers from the body.
         *
         * Content-Length tells the client exactly how many bytes the body
         * is, so it knows when it has received everything.
         */
        const char *body = "<html><body><h1>Hello World!</h1>"
                           "<p>This is served from a C HTTP server.</p>"
                           "</body></html>";

        char response[BUFFER_SIZE];
        int response_len = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            strlen(body), body);

        /*
         * write() sends data to the client through the socket.
         * We send the entire response (headers + body) in one write.
         *
         * In production, you'd need to handle partial writes (when write()
         * sends fewer bytes than requested), but for now this works.
         */
        write(client_fd, response, response_len);

        /*
         * STEP 7: Close the connection
         *
         * We're done with this client. Close their file descriptor.
         * The socket enters TIME_WAIT state for ~60 seconds (that's
         * normal TCP behavior), then is fully released.
         */
        close(client_fd);
        printf("[-] Connection closed\n\n");
    }

    /*
     * This line is never reached because of the infinite loop above.
     * In a real server, you'd handle SIGINT (Ctrl+C) to break out
     * of the loop and close the server socket gracefully.
     * We'll add that in a later step.
     */
    close(server_fd);
    return 0;
}
