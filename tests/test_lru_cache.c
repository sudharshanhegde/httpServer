/*
 * test_lru_cache.c - End-to-end tests for the thread-safe LRU
 * cache (src/lru_cache.c).
 *
 * The single-threaded logic (eviction order, capacity accounting, stats,
 * refcount-deferred frees) is tested deterministically first; then a real
 * multi-threaded stress test hammers the cache from several threads (no
 * sleep()-based synchronization — a pthread barrier coordinates the start).
 */

#include "lru_cache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

/* ---- payload helpers ---------------------------------------------------- */

static unsigned char *make_buf(size_t n, unsigned seed)
{
    unsigned char *b = malloc(n);
    for (size_t i = 0; i < n; i++) {
        b[i] = (unsigned char)(seed + i * 31 + 7);
    }
    return b;
}

static bool check_buf(const unsigned char *b, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++) {
        if (b[i] != (unsigned char)(seed + i * 31 + 7)) {
            return false;
        }
    }
    return true;
}

/* ---- deterministic single-threaded tests -------------------------------- */

static void test_put_get_and_miss(void)
{
    struct lru_cache c;
    CHECK(lru_cache_init(&c, 100000, 100000));

    CHECK(lru_cache_put(&c, "a", make_buf(8, 1), 8) == true);
    CHECK(lru_cache_put(&c, "b", make_buf(8, 2), 8) == true);

    struct lru_entry *e = lru_cache_get(&c, "a");
    CHECK(e != NULL);
    CHECK(e->size == 8);
    CHECK(check_buf(e->data, e->size, 1));
    lru_cache_release(&c, e);

    e = lru_cache_get(&c, "b");
    CHECK(e != NULL && check_buf(e->data, e->size, 2));
    lru_cache_release(&c, e);

    CHECK(lru_cache_get(&c, "missing") == NULL);

    unsigned long long hits, misses, ev;
    lru_cache_stats(&c, &hits, &misses, &ev);
    CHECK(hits == 2);
    CHECK(misses == 1);
    CHECK(ev == 0);

    lru_cache_destroy(&c);
}

static void test_overwrite_replaces_payload(void)
{
    struct lru_cache c;
    CHECK(lru_cache_init(&c, 100000, 100000));

    CHECK(lru_cache_put(&c, "k", make_buf(8, 10), 8) == true);
    CHECK(lru_cache_size(&c) == 8);
    CHECK(lru_cache_put(&c, "k", make_buf(16, 20), 16) == true);

    CHECK(lru_cache_entries(&c) == 1);   /* overwrite does not add an entry */
    CHECK(lru_cache_size(&c) == 16);     /* old 8 bytes freed, 16 added */

    struct lru_entry *e = lru_cache_get(&c, "k");
    CHECK(e != NULL && e->size == 16 && check_buf(e->data, e->size, 20));
    lru_cache_release(&c, e);

    lru_cache_destroy(&c);
}

static void test_eviction_by_entry_count(void)
{
    struct lru_cache c;
    CHECK(lru_cache_init(&c, 100000, 2)); /* at most 2 entries */

    CHECK(lru_cache_put(&c, "a", make_buf(8, 1), 8) == true);
    CHECK(lru_cache_put(&c, "b", make_buf(8, 2), 8) == true);
    CHECK(lru_cache_put(&c, "c", make_buf(8, 3), 8) == true); /* evicts "a" */

    CHECK(lru_cache_entries(&c) == 2);
    CHECK(lru_cache_get(&c, "a") == NULL);  /* LRU evicted */
    struct lru_entry *e = lru_cache_get(&c, "b");
    CHECK(e != NULL && check_buf(e->data, e->size, 2));
    lru_cache_release(&c, e);
    e = lru_cache_get(&c, "c");
    CHECK(e != NULL && check_buf(e->data, e->size, 3));
    lru_cache_release(&c, e);

    lru_cache_destroy(&c);
}

static void test_eviction_by_byte_capacity(void)
{
    struct lru_cache c;
    CHECK(lru_cache_init(&c, 25, 100000)); /* at most 25 bytes */

    CHECK(lru_cache_put(&c, "a", make_buf(10, 1), 10) == true);
    CHECK(lru_cache_put(&c, "b", make_buf(10, 2), 10) == true);
    CHECK(lru_cache_size(&c) == 20);

    /* Adding 10 more bytes (total would be 30 > 25) evicts the LRU first. */
    CHECK(lru_cache_put(&c, "c", make_buf(10, 3), 10) == true);
    CHECK(lru_cache_size(&c) <= 25);
    CHECK(lru_cache_get(&c, "a") == NULL);  /* evicted */
    CHECK(lru_cache_entries(&c) == 2);

    lru_cache_destroy(&c);
}

static void test_oversize_item_not_cached(void)
{
    struct lru_cache c;
    CHECK(lru_cache_init(&c, 10, 100000));

    /* Larger than the whole cache: rejected, data freed by the cache. */
    CHECK(lru_cache_put(&c, "big", make_buf(64, 9), 64) == false);
    CHECK(lru_cache_get(&c, "big") == NULL);
    CHECK(lru_cache_size(&c) == 0);

    lru_cache_destroy(&c);
}

static void test_refcount_defers_eviction_free(void)
{
    struct lru_cache c;
    CHECK(lru_cache_init(&c, 100000, 1)); /* only one entry allowed */

    CHECK(lru_cache_put(&c, "a", make_buf(8, 1), 8) == true);
    struct lru_entry *held = lru_cache_get(&c, "a"); /* refcount 1 */
    CHECK(held != NULL);

    /* Inserting "b" evicts "a", but "a" is still referenced. */
    CHECK(lru_cache_put(&c, "b", make_buf(8, 2), 8) == true);
    CHECK(lru_cache_get(&c, "a") == NULL);      /* already evicted */

    /* The held pointer must still be valid until release. */
    CHECK(check_buf(held->data, held->size, 1));
    lru_cache_release(&c, held);

    /* Now fully gone: no use-after-free, and a fresh get misses. */
    CHECK(lru_cache_get(&c, "a") == NULL);
    struct lru_entry *e = lru_cache_get(&c, "b");
    CHECK(e != NULL && check_buf(e->data, e->size, 2));
    lru_cache_release(&c, e);

    lru_cache_destroy(&c);
}

static void test_stats_after_eviction(void)
{
    struct lru_cache c;
    CHECK(lru_cache_init(&c, 100000, 2));

    CHECK(lru_cache_put(&c, "a", make_buf(4, 1), 4) == true);
    CHECK(lru_cache_put(&c, "b", make_buf(4, 2), 4) == true);
    CHECK(lru_cache_put(&c, "c", make_buf(4, 3), 4) == true); /* evicts "a" */

    struct lru_entry *e = lru_cache_get(&c, "c");
    lru_cache_release(&c, e);
    CHECK(lru_cache_get(&c, "a") == NULL); /* miss */

    unsigned long long hits, misses, ev;
    lru_cache_stats(&c, &hits, &misses, &ev);
    CHECK(hits == 1);
    CHECK(misses == 1);
    CHECK(ev == 1);

    lru_cache_destroy(&c);
}

/* ---- concurrent stress tests -------------------------------------------- */

enum { NTHREADS = 8, PER_THREAD = 100, NSHARED = 32 };

struct concurrent_ctx {
    struct lru_cache *cache;
    int tid;
    pthread_barrier_t *barrier;
    int errors;
};

static void *integrity_worker(void *arg)
{
    struct concurrent_ctx *ctx = arg;
    struct lru_cache *c = ctx->cache;
    pthread_barrier_wait(ctx->barrier);

    char key[64];
    /* Insert this thread's own distinct keys, then read them back. */
    for (int i = 0; i < PER_THREAD; i++) {
        snprintf(key, sizeof(key), "t%d-k%d", ctx->tid, i);
        if (!lru_cache_put(c, key, make_buf(16, 1000 + ctx->tid * PER_THREAD + i), 16)) {
            ctx->errors++;
        }
    }
    for (int i = 0; i < PER_THREAD; i++) {
        snprintf(key, sizeof(key), "t%d-k%d", ctx->tid, i);
        struct lru_entry *e = lru_cache_get(c, key);
        if (!e || !check_buf(e->data, e->size, 1000 + ctx->tid * PER_THREAD + i)) {
            ctx->errors++;
        }
        if (e) {
            lru_cache_release(c, e);
        }
    }
    /* Read-only access to shared keys (exercises get/release contention). */
    for (int i = 0; i < NSHARED; i++) {
        snprintf(key, sizeof(key), "shared-%d", i);
        struct lru_entry *e = lru_cache_get(c, key);
        if (!e || !check_buf(e->data, e->size, 500 + i)) {
            ctx->errors++;
        }
        if (e) {
            lru_cache_release(c, e);
        }
    }
    return NULL;
}

static void test_concurrent_integrity(void)
{
    /* Large capacity so no eviction happens; validates mutex correctness. */
    struct lru_cache c;
    CHECK(lru_cache_init(&c, 10000000, 1000000));

    char key[64];
    for (int i = 0; i < NSHARED; i++) {
        snprintf(key, sizeof(key), "shared-%d", i);
        CHECK(lru_cache_put(&c, key, make_buf(16, 500 + i), 16) == true);
    }

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, NTHREADS);

    pthread_t threads[NTHREADS];
    struct concurrent_ctx ctx[NTHREADS];
    for (int t = 0; t < NTHREADS; t++) {
        ctx[t].cache = &c;
        ctx[t].tid = t;
        ctx[t].barrier = &barrier;
        ctx[t].errors = 0;
        pthread_create(&threads[t], NULL, integrity_worker, &ctx[t]);
    }
    for (int t = 0; t < NTHREADS; t++) {
        pthread_join(threads[t], NULL);
        CHECK(ctx[t].errors == 0);
    }

    /* All entries must have survived with intact content. */
    CHECK(lru_cache_entries(&c) == (size_t)(NSHARED + NTHREADS * PER_THREAD));
    for (int t = 0; t < NTHREADS; t++) {
        for (int i = 0; i < PER_THREAD; i++) {
            snprintf(key, sizeof(key), "t%d-k%d", t, i);
            struct lru_entry *e = lru_cache_get(&c, key);
            CHECK(e != NULL && check_buf(e->data, e->size, 1000 + t * PER_THREAD + i));
            if (e) {
                lru_cache_release(&c, e);
            }
        }
    }

    pthread_barrier_destroy(&barrier);
    lru_cache_destroy(&c);
}

static void *eviction_stress_worker(void *arg)
{
    struct concurrent_ctx *ctx = arg;
    struct lru_cache *c = ctx->cache;
    pthread_barrier_wait(ctx->barrier);

    char key[64];
    for (int i = 0; i < 400; i++) {
        /* Churn through many distinct keys to force heavy eviction. */
        snprintf(key, sizeof(key), "t%d-w%d", ctx->tid, i);
        lru_cache_put(c, key, make_buf(8, i), 8);
        struct lru_entry *e = lru_cache_get(c, key);
        if (e) {
            lru_cache_release(c, e);
        }
    }
    return NULL;
}

static void test_concurrent_eviction_stress(void)
{
    struct lru_cache c;
    CHECK(lru_cache_init(&c, 1024 * 1024, 16)); /* tiny entry cap => evictions */

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, NTHREADS);

    pthread_t threads[NTHREADS];
    struct concurrent_ctx ctx[NTHREADS];
    for (int t = 0; t < NTHREADS; t++) {
        ctx[t].cache = &c;
        ctx[t].tid = t;
        ctx[t].barrier = &barrier;
        ctx[t].errors = 0;
        pthread_create(&threads[t], NULL, eviction_stress_worker, &ctx[t]);
    }
    for (int t = 0; t < NTHREADS; t++) {
        pthread_join(threads[t], NULL);
    }

    /* Capacity invariants must hold after all the churn. */
    CHECK(lru_cache_entries(&c) <= 16);
    CHECK(lru_cache_size(&c) <= 1024 * 1024);

    pthread_barrier_destroy(&barrier);
    lru_cache_destroy(&c);
}

int main(void)
{
    test_put_get_and_miss();
    test_overwrite_replaces_payload();
    test_eviction_by_entry_count();
    test_eviction_by_byte_capacity();
    test_oversize_item_not_cached();
    test_refcount_defers_eviction_free();
    test_stats_after_eviction();
    test_concurrent_integrity();
    test_concurrent_eviction_stress();

    if (g_failures == 0) {
        printf("test_lru_cache: all tests passed\n");
        return 0;
    }
    fprintf(stderr, "test_lru_cache: %d test(s) FAILED\n", g_failures);
    return 1;
}
