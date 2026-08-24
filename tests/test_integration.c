/*
 * test_integration.c - Checkpoint 7 end-to-end integration test.
 *
 * This wires the real modules together and drives a running pool with real
 * socket clients: full request lifecycle (GET / HEAD / keep-alive / 404 /
 * malformed), concurrency (many threads), and soak (many sequential requests).
 * It also exercises the previously-untested src/config.c (parsing a config
 * file) and src/logger.c (logging to a file) and verifies the log was written.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The globals that main.c normally owns; the test supplies its own. */
struct server_config g_config;
volatile bool g_shutdown = false;
FILE *g_log_file = NULL;

static int g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static int g_port = 0;

/* ---- tiny HTTP client ---------------------------------------------------- */

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
    size_t len = strlen(data), off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
}

/* Read one full response (header + Content-Length body). Heap string or NULL. */
static char *read_response(int fd)
{
    size_t cap = 65536, n = 0;
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
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) {
            break;
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
    return buf;
}

/* ---- tests --------------------------------------------------------------- */

static void test_lifecycle(void)
{
    /* GET / */
    int fd = tcp_connect();
    CHECK(fd >= 0);
    send_all(fd, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    char *r = read_response(fd);
    close(fd);
    CHECK(r != NULL && strstr(r, "HTTP/1.1 200 OK") != NULL);
    free(r);

    /* HEAD: 200, Content-Length, no body */
    fd = tcp_connect();
    CHECK(fd >= 0);
    send_all(fd, "HEAD / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    r = read_response(fd);
    close(fd);
    CHECK(r != NULL && strstr(r, "HTTP/1.1 200 OK") != NULL);
    if (r) {
        CHECK(strstr(r, "Content-Length:") != NULL);
        CHECK(strlen(strstr(r, "\r\n\r\n") + 4) == 0);
    }
    free(r);

    /* keep-alive: two requests on one connection */
    fd = tcp_connect();
    CHECK(fd >= 0);
    send_all(fd, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    r = read_response(fd);
    CHECK(r != NULL && strstr(r, "HTTP/1.1 200 OK") != NULL);
    free(r);
    send_all(fd, "GET /nope HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    r = read_response(fd);
    close(fd);
    CHECK(r != NULL && strstr(r, "HTTP/1.1 404 Not Found") != NULL);
    free(r);

    /* malformed -> 400 */
    fd = tcp_connect();
    CHECK(fd >= 0);
    send_all(fd, "\r\n\r\n");
    r = read_response(fd);
    close(fd);
    CHECK(r != NULL && strstr(r, "HTTP/1.1 400 Bad Request") != NULL);
    free(r);
}

static void *burst_worker(void *arg)
{
    (void)arg;
    int ok = 1;
    for (int i = 0; i < 25; i++) {
        int fd = tcp_connect();
        if (fd < 0) {
            ok = 0;
            break;
        }
        send_all(fd, "GET /index.html HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        char *r = read_response(fd);
        close(fd);
        if (!r || strstr(r, "HTTP/1.1 200 OK") == NULL) {
            ok = 0;
        }
        free(r);
    }
    return (void *)(long)!ok;
}

static void test_concurrency(void)
{
    enum { NTHREADS = 8 };
    pthread_t threads[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        pthread_create(&threads[i], NULL, burst_worker, NULL);
    }
    int failures = 0;
    for (int i = 0; i < NTHREADS; i++) {
        void *ret;
        pthread_join(threads[i], &ret);
        failures += (int)(long)ret;
    }
    CHECK(failures == 0);
}

static void test_soak(void)
{
    int ok = 1;
    for (int i = 0; i < 300; i++) {
        int fd = tcp_connect();
        if (fd < 0) {
            ok = 0;
            break;
        }
        send_all(fd, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        char *r = read_response(fd);
        close(fd);
        if (!r || strstr(r, "HTTP/1.1 200 OK") == NULL) {
            ok = 0;
        }
        free(r);
    }
    CHECK(ok == 1);
}

int main(void)
{
    char dir[] = "/tmp/httpd_it_XXXXXX";
    char *d = mkdtemp(dir);
    CHECK(d != NULL);

    char cfg_path[512], log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/server.log", d);
    snprintf(cfg_path, sizeof(cfg_path), "%s/server.conf", d);

    /* Write a config file and load it (exercises config.c). */
    FILE *cf = fopen(cfg_path, "w");
    CHECK(cf != NULL);
    if (cf) {
        fprintf(cf,
                "port = 9999\n"
                "server_root = ./www\n"
                "num_threads = 6\n"
                "log_file = %s\n"
                "timeout_secs = 15\n",
                log_path);
        fclose(cf);
    }

    config_load(cfg_path, &g_config);
    CHECK(g_config.port == 9999);
    CHECK(g_config.num_threads == 6);
    CHECK(g_config.timeout_secs == 15);
    CHECK(strcmp(g_config.server_root, "./www") == 0);
    CHECK(strcmp(g_config.log_file, log_path) == 0);
    config_print(&g_config); /* exercised for coverage */

    /* Use an ephemeral port for the test to avoid conflicts. */
    g_config.port = 0;

    log_init(g_config.log_file);                 /* exercises logger.c */
    log_msg("INFO", "integration test marker");  /* exercises logger.c */

    struct pool_config pc;
    memset(&pc, 0, sizeof(pc));
    pc.port = g_config.port;
    pc.doc_root = g_config.server_root;
    pc.min_workers = 1;
    pc.max_workers = g_config.num_threads;
    pc.load_high = 8;
    pc.load_low = 1;
    pc.tune_interval_ms = 20;

    struct thread_pool *pool = pool_create(&pc);
    CHECK(pool != NULL);
    g_port = pool_port(pool);
    CHECK(g_port > 0);

    test_lifecycle();
    test_concurrency();
    test_soak();

    log_request("127.0.0.1", "GET", "/", 200, 12); /* exercises log_request */
    pool_destroy(pool);
    log_close();

    /* The log file must contain startup + our marker. */
    char *logbuf = malloc(1 << 20);
    CHECK(logbuf != NULL);
    size_t rn = 0;
    FILE *lf = fopen(log_path, "r");
    CHECK(lf != NULL);
    if (lf) {
        rn = fread(logbuf, 1, (1 << 20) - 1, lf);
        logbuf[rn] = '\0';
        fclose(lf);
    }
    CHECK(strstr(logbuf, "Server is Starting") != NULL);
    CHECK(strstr(logbuf, "integration test marker") != NULL);
    CHECK(strstr(logbuf, "Server is shutting down") != NULL);
    free(logbuf);

    unlink(cfg_path);
    unlink(log_path);
    rmdir(d);

    if (g_failures == 0) {
        printf("test_integration: all tests passed\n");
        return 0;
    }
    fprintf(stderr, "test_integration: %d test(s) FAILED\n", g_failures);
    return 1;
}
