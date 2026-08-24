/*
 * reactor.h - Single-threaded, edge-triggered epoll HTTP reactor.
 *
 * This is Checkpoint 4: a non-blocking event loop that accepts connections,
 * reads requests incrementally (edge-triggered: drain until EAGAIN), parses
 * them with the Checkpoint 1 state machine, and writes responses handling
 * partial writes and keep-alive. It is deliberately single-threaded — the
 * worker pool (Checkpoint 5) will distribute connections/events on top of it.
 *
 * The body-transmission path here is a naive read()+write() into memory; the
 * sendfile() zero-copy data plane (Checkpoint 6) replaces that strategy behind
 * the same reactor structure.
 */

#ifndef REACTOR_H
#define REACTOR_H

#include "server.h" /* http_request, http_parser, constants */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Tunables for the reactor. Fields with 0 select a sane default. */
struct reactor_config {
    int port;            /* 0 = bind an ephemeral port (retrieve via reactor_port) */
    const char *doc_root; /* directory served as "/" (NULL -> "www") */
    int max_events;      /* epoll_wait batch size (0 -> MAX_EVENTS) */
    int backlog;         /* listen() backlog (0 -> BACKLOG) */
    bool reuse_port;     /* set SO_REUSEPORT so several reactors share one port */
};

/** Opaque reactor handle. */
struct reactor;

/**
 * reactor_create - Bind a listening socket and set up the epoll instance.
 *
 * The listen socket and the shutdown eventfd are registered with epoll but the
 * loop does not start until reactor_run(). Call reactor_port() to learn the
 * actual bound port when @cfg.port was 0.
 *
 * @cfg: Configuration (may not be NULL).
 *
 * Returns: an initialized reactor, or NULL on any allocation/socket failure.
 */
struct reactor *reactor_create(const struct reactor_config *cfg);

/**
 * reactor_port - The actual port the reactor's listening socket is bound to.
 *
 * @r: Reactor from reactor_create().
 *
 * Returns: the bound port.
 */
int reactor_port(const struct reactor *r);

/**
 * reactor_active_connections - Number of live client connections.
 *
 * Maintained as an atomic counter, so it is safe to read from another thread
 * (the worker-pool tuner reads it to decide whether to scale up/down).
 *
 * @r: Reactor from reactor_create().
 *
 * Returns: the current number of active connections.
 */
int reactor_active_connections(const struct reactor *r);

/**
 * reactor_run - Run the event loop until reactor_stop() is called.
 *
 * Blocking. Intended to be run in its own thread (or the main thread). Returns
 * when reactor_stop() is invoked from another thread.
 *
 * @r: Reactor from reactor_create().
 */
void reactor_run(struct reactor *r);

/**
 * reactor_stop - Ask the event loop to exit.
 *
 * Wakes the blocked epoll_wait via an eventfd and causes reactor_run() to
 * return. Safe to call from another thread.
 *
 * @r: Reactor from reactor_create().
 */
void reactor_stop(struct reactor *r);

/**
 * reactor_destroy - Tear down the reactor, closing the listen socket and
 * epoll instance.
 *
 * Call after reactor_run() has returned and all connections are done.
 *
 * @r: Reactor from reactor_create(). May be NULL.
 */
void reactor_destroy(struct reactor *r);

#endif /* REACTOR_H */
