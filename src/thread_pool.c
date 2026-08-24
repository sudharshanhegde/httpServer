/*
 * thread_pool.c - Dynamically tuned worker pool over per-thread epoll reactors.
 *
 * Each worker runs its own reactor (own epoll + own listen socket). All
 * workers bind the same port with SO_REUSEPORT, so the kernel load-balances
 * new connections across them. A monitor thread samples the total active-
 * connection count and scales the pool between min and max: add a worker above
 * the high watermark, retire an idle worker below the low watermark.
 *
 * This supersedes the original fixed-size, blocking thread-pool model.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "thread_pool.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct worker {
    int id;
    struct reactor *reactor;
    pthread_t thread;
};

struct thread_pool {
    struct pool_config cfg;
    int port;
    struct worker *workers; /* array of capacity max_workers */
    int num_workers;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t monitor;
    volatile bool stop;
};

/* Pick an ephemeral port by binding a temporary socket and reading it back. */
static int pick_ephemeral_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return 0;
    }
    socklen_t len = sizeof(sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &len) < 0) {
        close(fd);
        return 0;
    }
    int port = ntohs(sa.sin_port);
    close(fd);
    return port;
}

static void *worker_fn(void *arg)
{
    struct reactor *r = arg;
    reactor_run(r);
    return NULL;
}

/* Assumes pool->lock is held. Adds one worker bound to pool->port. */
static void spawn_worker(struct thread_pool *pool)
{
    if (pool->num_workers >= pool->cfg.max_workers) {
        return;
    }
    struct reactor_config rc;
    memset(&rc, 0, sizeof(rc));
    rc.port = pool->port;
    rc.doc_root = pool->cfg.doc_root;
    rc.reuse_port = true;

    struct reactor *r = reactor_create(&rc);
    if (!r) {
        return;
    }
    int idx = pool->num_workers;
    pool->workers[idx].id = idx;
    pool->workers[idx].reactor = r;
    if (pthread_create(&pool->workers[idx].thread, NULL, worker_fn, r) != 0) {
        reactor_destroy(r);
        return;
    }
    pool->num_workers++;
}

/* Assumes pool->lock is held. Retires the first idle worker if above min. */
static void retire_idle_worker(struct thread_pool *pool)
{
    if (pool->num_workers <= pool->cfg.min_workers) {
        return;
    }
    for (int i = 0; i < pool->num_workers; i++) {
        if (reactor_active_connections(pool->workers[i].reactor) == 0) {
            reactor_stop(pool->workers[i].reactor);
            pthread_join(pool->workers[i].thread, NULL);
            reactor_destroy(pool->workers[i].reactor);
            /* Swap the retired slot with the last worker. */
            pool->workers[i] = pool->workers[pool->num_workers - 1];
            pool->workers[i].id = i;
            pool->num_workers--;
            return;
        }
    }
}

/* Assumes pool->lock is held. Sample total load and scale the pool. */
static void tune_pool(struct thread_pool *pool)
{
    int total = 0;
    for (int i = 0; i < pool->num_workers; i++) {
        total += reactor_active_connections(pool->workers[i].reactor);
    }
    if (total > pool->cfg.load_high) {
        spawn_worker(pool);
    } else if (total < pool->cfg.load_low) {
        retire_idle_worker(pool);
    }
}

static void *monitor_fn(void *arg)
{
    struct thread_pool *pool = arg;

    for (;;) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long ms = pool->cfg.tune_interval_ms > 0 ? pool->cfg.tune_interval_ms : 50;
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&pool->lock);
        while (!pool->stop) {
            int rc = pthread_cond_timedwait(&pool->cond, &pool->lock, &ts);
            if (pool->stop || rc == ETIMEDOUT) {
                break;
            }
        }
        if (pool->stop) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }
        tune_pool(pool);
        pthread_mutex_unlock(&pool->lock);
    }
    return NULL;
}

struct thread_pool *pool_create(const struct pool_config *cfg)
{
    if (!cfg || cfg->min_workers < 1 || cfg->max_workers < cfg->min_workers) {
        return NULL;
    }

    struct thread_pool *pool = calloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }
    pool->cfg = *cfg;
    pool->workers = calloc((size_t)cfg->max_workers, sizeof(struct worker));
    if (!pool->workers) {
        free(pool);
        return NULL;
    }
    pool->port = (cfg->port > 0) ? cfg->port : pick_ephemeral_port();
    if (pool->port <= 0) {
        free(pool->workers);
        free(pool);
        return NULL;
    }
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);

    /* Start the baseline workers. */
    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < cfg->min_workers; i++) {
        spawn_worker(pool);
    }
    pthread_mutex_unlock(&pool->lock);

    pthread_create(&pool->monitor, NULL, monitor_fn, pool);
    return pool;
}

int pool_port(const struct thread_pool *pool)
{
    return pool->port;
}

int pool_worker_count(const struct thread_pool *pool)
{
    return pool->num_workers;
}

void pool_destroy(struct thread_pool *pool)
{
    if (!pool) {
        return;
    }

    pthread_mutex_lock(&pool->lock);
    pool->stop = true;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);

    pthread_join(pool->monitor, NULL);

    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->num_workers; i++) {
        reactor_stop(pool->workers[i].reactor);
    }
    for (int i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->workers[i].thread, NULL);
    }
    for (int i = 0; i < pool->num_workers; i++) {
        reactor_destroy(pool->workers[i].reactor);
    }
    pool->num_workers = 0;
    pthread_mutex_unlock(&pool->lock);

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
    free(pool->workers);
    free(pool);
}
