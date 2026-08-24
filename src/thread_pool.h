/*
 * thread_pool.h - Dynamically tuned worker pool over per-thread epoll reactors.
 *
 * Checkpoint 5: the single-threaded reactor (Checkpoint 4) is wrapped in a pool
 * that distributes incoming connections across several worker threads. Each
 * worker owns its own reactor (its own epoll instance + its own listen socket),
 * and all workers share one port via SO_REUSEPORT so the kernel spreads
 * connections among them.
 *
 * A monitor thread watches the total active-connection count and dynamically
 * sizes the pool: it spawns a new worker when load rises above a high
 * watermark (up to @max_workers) and retires idle workers when load falls
 * below a low watermark (down to @min_workers). This supersedes the earlier
 * fixed-size, blocking per-worker model.
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "reactor.h"

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/** Tunables for the pool. */
struct pool_config {
    int port;               /* shared port; 0 = pick an ephemeral one */
    const char *doc_root;   /* directory served as "/" (NULL -> "www") */
    int min_workers;        /* pool never shrinks below this */
    int max_workers;        /* pool never grows above this */
    int load_high;          /* total active conns above which to add a worker */
    int load_low;           /* total active conns below which to retire idle */
    int tune_interval_ms;   /* how often the monitor re-evaluates load */
};

/** Opaque pool handle. */
struct thread_pool;

/**
 * pool_create - Start the pool: bind @min_workers reactors on one shared port
 * and launch the scaling monitor.
 *
 * @cfg: Configuration (may not be NULL).
 *
 * Returns: an initialized pool, or NULL on failure.
 */
struct thread_pool *pool_create(const struct pool_config *cfg);

/**
 * pool_port - The port all workers listen on.
 *
 * @pool: Pool from pool_create().
 *
 * Returns: the shared bound port.
 */
int pool_port(const struct thread_pool *pool);

/**
 * pool_worker_count - Current number of worker threads.
 *
 * Approximate; the monitor changes it asynchronously.
 *
 * @pool: Pool from pool_create().
 *
 * Returns: the current worker count.
 */
int pool_worker_count(const struct thread_pool *pool);

/**
 * pool_destroy - Stop all workers and the monitor, joining their threads.
 *
 * Blocks until every worker and the monitor have exited.
 *
 * @pool: Pool from pool_create(). May be NULL.
 */
void pool_destroy(struct thread_pool *pool);

#endif /* THREAD_POOL_H */
