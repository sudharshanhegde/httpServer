/*
 * main.c - Entry point that wires the worker pool + reactor + sendfile data
 * plane into a runnable server.
 *
 * Loads the configuration file, initializes logging, starts the dynamically
 * tuned worker pool, and runs until SIGINT/SIGTERM triggers a clean shutdown
 * (stop the pool, close the log).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server.h"
#include "thread_pool.h"

#include <string.h>

struct server_config g_config;
volatile bool g_shutdown = false;
FILE *g_log_file = NULL;

void signal_handler(int sig)
{
    (void)sig;
    g_shutdown = true;
}

void setup_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int main(int argc, char **argv)
{
    const char *conf = (argc > 1) ? argv[1] : "server.conf";

    config_load(conf, &g_config);
    log_init(g_config.log_file);
    config_print(&g_config);
    setup_signal_handlers();

    struct pool_config pc;
    memset(&pc, 0, sizeof(pc));
    pc.port = g_config.port;
    pc.doc_root = g_config.server_root;
    pc.min_workers = 1;
    pc.max_workers = g_config.num_threads > 0 ? g_config.num_threads : DEFAULT_THREADS;
    pc.load_high = pc.max_workers * 4; /* scale up under meaningful load */
    pc.load_low = 1;
    pc.tune_interval_ms = 200;

    struct thread_pool *pool = pool_create(&pc);
    if (!pool) {
        log_msg("ERROR", "Failed to start server pool");
        log_close();
        return 1;
    }
    log_msg("INFO", "Server listening on port %d (max %d workers)",
            pool_port(pool), g_config.num_threads);

    while (!g_shutdown) {
        pause();
    }

    log_msg("INFO", "Shutting down");
    pool_destroy(pool);
    log_close();
    return 0;
}
