/*
 * test_reactor.c - End-to-end tests for the edge-triggered epoll
 * reactor (src/reactor.c).
 *
 * The reactor is started in a background thread bound to a real ephemeral
 * port, and driven by REAL sockets (a small C client connecting over
 * loopback) — no fake epoll, no mocks. Each named test exercises a distinct
 * reactor behavior: serving, 404, HEAD, keep-alive, partial reads, malformed
 * input, path traversal, and concurrent connections.
 */

#include "reactor.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static struct reactor *g_reactor = NULL;
static int g_port = 0;

/* ---- small HTTP client helpers ------------------------------------------- */

static int tcp_connect(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void send_all(int fd, const char *data)
{
    size_t len = strlen(data);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
}

/*
 * Read one full HTTP response off @fd: header up to "\r\n\r\n", then
 * Content-Length body bytes. Returns the response as a heap string, or NULL on
 * error.
 */
static char *read_response(int fd)
{
    size_t cap = 65536;
    size_t n = 0;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    size_t hdr_end = 0;

    while (1) {
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        /* Leave room for the trailing NUL at buf[n]. */
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) {
            break; /* EOF or error */
        }
        n += (size_t)r;
        buf[n] = '\0';

        if (hdr_end == 0) {
            for (size_t i = 0; i + 3 < n; i++) {
                if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                    buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                    hdr_end = i + 4;
                    break;
                }
            }
        }
        if (hdr_end != 0) {
            /* Find Content-Length in the header portion. */
            size_t clen = 0;
            char *cl = strstr(buf, "Content-Length:");
            if (cl && (size_t)(cl - buf) < hdr_end) {
                clen = (size_t)strtoul(cl + 15, NULL, 10);
            }
            if (n >= hdr_end + clen) {
                return buf;
            }
        }
    }
    /* EOF without a complete body (only valid for Connection: close). */
    return buf;
}

static void *reactor_thread(void *arg)
{
    (void)arg;
    reactor_run(g_reactor);
    return NULL;
}

static void start_server(void)
{
    struct reactor_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;              /* ephemeral */
    cfg.doc_root = "www";      /* served relative to the workspace root */
    g_reactor = reactor_create(&cfg);
    CHECK(g_reactor != NULL);
    g_port = reactor_port(g_reactor);
    CHECK(g_port > 0);

    pthread_t t;
    pthread_create(&t, NULL, reactor_thread, NULL);
    /* Detach so teardown only needs reactor_stop + a short settle. */
    pthread_detach(t);
    usleep(100 * 1000); /* give the loop time to start */
}

static void stop_server(void)
{
    reactor_stop(g_reactor);
    usleep(100 * 1000);
    reactor_destroy(g_reactor);
    g_reactor = NULL;
}

/* ---- tests --------------------------------------------------------------- */

static void test_serve_index(void)
{
    int fd = tcp_connect();
    CHECK(fd >= 0);
    send_all(fd, "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    char *resp = read_response(fd);
    close(fd);
    CHECK(resp != NULL);
    if (resp) {
        CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
        CHECK(strstr(resp, "Content-Type: text/html") != NULL);
        CHECK(strstr(resp, "HTTP server is working") != NULL); /* body served */
    }
    free(resp);
}

static void test_not_found(void)
{
    int fd = tcp_connect();
    CHECK(fd >= 0);
    send_all(fd, "GET /does-not-exist.txt HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    char *resp = read_response(fd);
    close(fd);
    CHECK(resp != NULL);
    if (resp) {
        CHECK(strstr(resp, "HTTP/1.1 404 Not Found") != NULL);
    }
    free(resp);
}

static void test_head_no_body(void)
{
    int fd = tcp_connect();
    CHECK(fd >= 0);
    send_all(fd, "HEAD / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    char *resp = read_response(fd);
    close(fd);
    CHECK(resp != NULL);
    if (resp) {
        CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
        CHECK(strstr(resp, "Content-Length:") != NULL);
        /* HEAD must carry a Content-Length but no body. */
        const char *body = strstr(resp, "\r\n\r\n") + 4;
        CHECK(strlen(body) == 0);
    }
    free(resp);
}

static void test_keep_alive_two_requests(void)
{
    int fd = tcp_connect();
    CHECK(fd >= 0);
    /* Two requests on one persistent connection (no Connection: close). */
    send_all(fd, "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
    char *r1 = read_response(fd);
    CHECK(r1 != NULL);
    if (r1) {
        CHECK(strstr(r1, "HTTP/1.1 200 OK") != NULL);
    }
    free(r1);

    send_all(fd, "GET /does-not-exist.txt HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    char *r2 = read_response(fd);
    close(fd);
    CHECK(r2 != NULL);
    if (r2) {
        CHECK(strstr(r2, "HTTP/1.1 404 Not Found") != NULL);
    }
    free(r2);
}

static void test_partial_request_reads(void)
{
    /* Send a request in two chunks to force a partial read in the reactor. */
    int fd = tcp_connect();
    CHECK(fd >= 0);
    send_all(fd, "GET / HTTP/1.1\r\nHo");
    usleep(30 * 1000);
    send_all(fd, "st: test\r\nConnection: close\r\n\r\n");
    char *resp = read_response(fd);
    close(fd);
    CHECK(resp != NULL);
    if (resp) {
        CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    }
    free(resp);
}

static void test_malformed_request(void)
{
    /* An empty request line is a parse error -> 400. */
    int fd = tcp_connect();
    CHECK(fd >= 0);
    send_all(fd, "\r\n\r\n");
    char *resp = read_response(fd);
    close(fd);
    CHECK(resp != NULL);
    if (resp) {
        CHECK(strstr(resp, "HTTP/1.1 400 Bad Request") != NULL);
    }
    free(resp);
}

static void test_path_traversal_rejected(void)
{
    int fd = tcp_connect();
    CHECK(fd >= 0);
    send_all(fd, "GET /../etc/passwd HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    char *resp = read_response(fd);
    close(fd);
    CHECK(resp != NULL);
    if (resp) {
        CHECK(strstr(resp, "HTTP/1.1 403 Forbidden") != NULL);
    }
    free(resp);
}

static void *client_worker(void *arg)
{
    int port = *(int *)arg;
    /* A bespoke connection to avoid the shared globals. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    if (ok) {
        send_all(fd, "GET /index.html HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        char *resp = read_response(fd);
        if (!resp || strstr(resp, "HTTP/1.1 200 OK") == NULL) {
            ok = 0;
        }
        free(resp);
    }
    close(fd);
    return (void *)(long)!ok;
}

static void test_concurrent_connections(void)
{
    enum { N = 16 };
    pthread_t threads[N];
    for (int i = 0; i < N; i++) {
        pthread_create(&threads[i], NULL, client_worker, &g_port);
    }
    for (int i = 0; i < N; i++) {
        void *ret;
        pthread_join(threads[i], &ret);
        CHECK((long)ret == 0);
    }
}

/*
 * Validate the sendfile() data plane by serving an 8 MiB file
 * and verifying Content-Length and every body byte match what was written.
 */
static void test_large_file_sendfile(void)
{
    enum { SIZE = 8 * 1024 * 1024, CHUNK = 8192 };
    const char *path = "www/big.bin";

    unsigned char chunk[CHUNK];
    for (size_t i = 0; i < CHUNK; i++) {
        chunk[i] = (unsigned char)(i * 31 + 7);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    size_t written = 0;
    while (written < SIZE) {
        size_t n = (SIZE - written) < CHUNK ? (SIZE - written) : CHUNK;
        ssize_t w = write(fd, chunk, n);
        CHECK(w > 0);
        written += (size_t)w;
    }
    close(fd);

    int sock = tcp_connect();
    CHECK(sock >= 0);
    send_all(sock, "GET /big.bin HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    char *resp = read_response(sock);
    close(sock);
    CHECK(resp != NULL);
    if (resp) {
        CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
        char *cl = strstr(resp, "Content-Length:");
        CHECK(cl != NULL);
        if (cl) {
            CHECK((size_t)strtoul(cl + 15, NULL, 10) == SIZE);
        }
        /* read_response() returns only once it has hdr_end + Content-Length
         * bytes, so the body region holds all SIZE bytes. The pattern contains
         * NUL bytes, so compare against SIZE rather than strlen(). */
        const char *body = strstr(resp, "\r\n\r\n") + 4;
        size_t ok = 1;
        for (size_t i = 0; i < SIZE; i++) {
            if ((unsigned char)body[i] != (unsigned char)((i * 31 + 7) & 0xff)) {
                ok = 0;
                break;
            }
        }
        CHECK(ok == 1);
    }
    free(resp);
    unlink(path);
}

int main(void)
{
    start_server();

    test_serve_index();
    test_not_found();
    test_head_no_body();
    test_keep_alive_two_requests();
    test_partial_request_reads();
    test_malformed_request();
    test_path_traversal_rejected();
    test_concurrent_connections();
    test_large_file_sendfile();

    stop_server();

    if (g_failures == 0) {
        printf("test_reactor: all tests passed\n");
        return 0;
    }
    fprintf(stderr, "test_reactor: %d test(s) FAILED\n", g_failures);
    return 1;
}
