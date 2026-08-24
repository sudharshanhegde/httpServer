/*
 * test_thread_pool.c - Checkpoint 5 end-to-end tests for the dynamically tuned
 * worker pool (src/thread_pool.c).
 *
 * A pool of per-thread SO_REUSEPORT reactors runs on a real ephemeral port.
 * To observe dynamic scaling deterministically we hold several keep-alive
 * connections OPEN (coordinated with a pthread barrier + condvar, not sleep)
 * so the monitor samples a high active-connection count, verify the pool grows
 * above its minimum, then release the connections and verify it shrinks back,
 * and finally verify pool_destroy() shuts everything down cleanly.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "thread_pool.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

enum { NCLIENTS = 20 };

struct hold_ctx {
    int port;
    pthread_barrier_t start; /* NCLIENTS clients + main */
    pthread_mutex_t m;
    pthread_cond_t release;
    bool go;
};

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

/* Read the response header (up to "\r\n\r\n"); heap string or NULL. */
static char *read_header(int fd)
{
    size_t cap = 8192, n = 0;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    while (1) {
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r <= 0) {
            break;
        }
        n += (size_t)r;
        buf[n] = '\0';
        for (size_t i = 0; i + 3 < n; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                buf[i + 4] = '\0';
                return buf;
            }
        }
    }
    return buf;
}

/*
 * Connect, send a keep-alive GET, confirm 200, then HOLD the connection open
 * until main signals release. This keeps the connection active on the server
 * side so the pool's monitor observes sustained load.
 */
static void *hold_client(void *arg)
{
    struct hold_ctx *ctx = arg;
    int ok = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)ctx->port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        send_all(fd, "GET / HTTP/1.1\r\nHost: t\r\n\r\n"); /* keep-alive */
        char *resp = read_header(fd);
        if (resp && strstr(resp, "HTTP/1.1 200 OK") != NULL) {
            ok = 1;
        }
        free(resp);
    }

    /* Signal that this connection is established and being held open. */
    pthread_barrier_wait(&ctx->start);

    pthread_mutex_lock(&ctx->m);
    while (!ctx->go) {
        pthread_cond_wait(&ctx->release, &ctx->m);
    }
    pthread_mutex_unlock(&ctx->m);

    if (fd >= 0) {
        close(fd);
    }
    return (void *)(long)!ok;
}

/* Poll a predicate on the pool until it holds or the timeout elapses. */
static int wait_for(int (*pred)(struct thread_pool *, long), struct thread_pool *pool,
                    long arg, int timeout_ms)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (pred(pool, arg)) {
            return 1;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                       (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            return 0;
        }
        usleep(10 * 1000);
    }
}

static int above_min(struct thread_pool *pool, long min)
{
    return pool_worker_count(pool) > (int)min;
}

static int at_min(struct thread_pool *pool, long min)
{
    return pool_worker_count(pool) == (int)min;
}

int main(void)
{
    struct pool_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.doc_root = "www";
    cfg.min_workers = 2;
    cfg.max_workers = 8;
    cfg.load_high = 2; /* grow above this many active connections */
    cfg.load_low = 1;  /* retire below this many */
    cfg.tune_interval_ms = 20;

    struct thread_pool *pool = pool_create(&cfg);
    CHECK(pool != NULL);
    int port = pool_port(pool);
    CHECK(port > 0);
    CHECK(pool_worker_count(pool) == cfg.min_workers);

    struct hold_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.port = port;
    pthread_barrier_init(&ctx.start, NULL, NCLIENTS + 1);
    pthread_mutex_init(&ctx.m, NULL);
    pthread_cond_init(&ctx.release, NULL);

    pthread_t threads[NCLIENTS];
    for (int i = 0; i < NCLIENTS; i++) {
        pthread_create(&threads[i], NULL, hold_client, &ctx);
    }

    /* Wait until every client has established and is holding its connection. */
    pthread_barrier_wait(&ctx.start);

    /* With NCLIENTS connections active, the pool must grow above its minimum. */
    CHECK(wait_for(above_min, pool, cfg.min_workers, 3000));
    int grown = pool_worker_count(pool);
    CHECK(grown > cfg.min_workers);
    CHECK(grown <= cfg.max_workers);

    /* Release the connections so the pool can shed workers. */
    pthread_mutex_lock(&ctx.m);
    ctx.go = true;
    pthread_cond_broadcast(&ctx.release);
    pthread_mutex_unlock(&ctx.m);

    int client_failures = 0;
    for (int i = 0; i < NCLIENTS; i++) {
        void *ret;
        pthread_join(threads[i], &ret);
        client_failures += (int)(long)ret;
    }
    CHECK(client_failures == 0); /* every held connection was served 200 */

    /* Once idle, the pool must shrink back to the minimum. */
    CHECK(wait_for(at_min, pool, cfg.min_workers, 5000));
    int shrunk = pool_worker_count(pool);
    CHECK(shrunk == cfg.min_workers);

    pthread_barrier_destroy(&ctx.start);
    pthread_mutex_destroy(&ctx.m);
    pthread_cond_destroy(&ctx.release);

    pool_destroy(pool); /* must join all workers + monitor cleanly */

    if (g_failures == 0) {
        printf("test_thread_pool: all tests passed (grew to %d workers, shrank to %d)\n",
               grown, shrunk);
        return 0;
    }
    fprintf(stderr, "test_thread_pool: %d test(s) FAILED\n", g_failures);
    return 1;
}
