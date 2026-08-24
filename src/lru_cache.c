/*
 * lru_cache.c - Thread-safe LRU cache built on the open-addressing hash table.
 *
 * Recency is maintained with a doubly-linked list (head = MRU, tail = LRU);
 * O(1) lookup uses the generic hash table, which maps a key to its entry. All
 * public operations serialize on a single mutex. Evicted-but-referenced
 * entries are detached from the cache immediately but their memory is freed
 * only when the last reference is released (see struct lru_entry.refcount).
 */

#include "lru_cache.h"

#include <stdlib.h>
#include <string.h>

/* FNV-1a 64-bit string hash for the underlying table. */
static uint64_t key_hash(const void *key)
{
    const unsigned char *s = key;
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= *s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static bool key_eq(const void *a, const void *b)
{
    return strcmp(a, b) == 0;
}

/* ---- LRU list helpers (caller must hold the mutex) ---------------------- */

static void list_unlink(struct lru_cache *c, struct lru_entry *e)
{
    if (e->prev) {
        e->prev->next = e->next;
    } else {
        c->head = e->next;
    }
    if (e->next) {
        e->next->prev = e->prev;
    } else {
        c->tail = e->prev;
    }
    e->prev = NULL;
    e->next = NULL;
}

static void list_push_front(struct lru_cache *c, struct lru_entry *e)
{
    e->prev = NULL;
    e->next = c->head;
    if (c->head) {
        c->head->prev = e;
    }
    c->head = e;
    if (!c->tail) {
        c->tail = e;
    }
}

static void list_move_front(struct lru_cache *c, struct lru_entry *e)
{
    if (c->head == e) {
        return;
    }
    list_unlink(c, e);
    list_push_front(c, e);
}

static void entry_free(struct lru_entry *e)
{
    free(e->data);
    free(e->key);
    free(e);
}

/*
 * Detach an entry from the cache (table + list + accounting) without freeing
 * it. The payload is kept alive if a reader still holds a reference.
 */
static void entry_detach(struct lru_cache *c, struct lru_entry *e)
{
    ht_remove(&c->table, e->key);
    list_unlink(c, e);
    c->current_size -= e->size;
    c->num_entries--;
    e->removed = true;
}

/* Free an entry if it is both removed and has no outstanding references. */
static void entry_maybe_free(struct lru_entry *e)
{
    if (e->removed && e->refcount == 0) {
        entry_free(e);
    }
}

static void evict_lru(struct lru_cache *c)
{
    struct lru_entry *victim = c->tail;
    if (!victim) {
        return;
    }
    entry_detach(c, victim);
    c->evictions++;
    entry_maybe_free(victim);
}

/* ---- public API --------------------------------------------------------- */

bool lru_cache_init(struct lru_cache *c, size_t max_size, size_t max_entries)
{
    if (!c) {
        return false;
    }
    if (!ht_init(&c->table, 0, key_hash, key_eq)) {
        return false;
    }
    c->head = NULL;
    c->tail = NULL;
    c->current_size = 0;
    c->max_size = max_size;
    c->num_entries = 0;
    c->max_entries = max_entries;
    c->hits = 0;
    c->misses = 0;
    c->evictions = 0;
    pthread_mutex_init(&c->mutex, NULL);
    return true;
}

void lru_cache_destroy(struct lru_cache *c)
{
    pthread_mutex_lock(&c->mutex);

    struct lru_entry *e = c->head;
    while (e) {
        struct lru_entry *next = e->next;
        entry_free(e);
        e = next;
    }
    c->head = NULL;
    c->tail = NULL;
    c->num_entries = 0;
    c->current_size = 0;

    ht_destroy(&c->table);
    pthread_mutex_unlock(&c->mutex);
    pthread_mutex_destroy(&c->mutex);
}

struct lru_entry *lru_cache_get(struct lru_cache *c, const char *key)
{
    pthread_mutex_lock(&c->mutex);

    struct lru_entry *e = ht_lookup(&c->table, key);
    if (e) {
        e->refcount++;
        list_move_front(c, e);
        c->hits++;
        pthread_mutex_unlock(&c->mutex);
        return e;
    }

    c->misses++;
    pthread_mutex_unlock(&c->mutex);
    return NULL;
}

void lru_cache_release(struct lru_cache *c, struct lru_entry *e)
{
    if (!e) {
        return;
    }
    pthread_mutex_lock(&c->mutex);
    if (e->refcount > 0) {
        e->refcount--;
    }
    entry_maybe_free(e);
    pthread_mutex_unlock(&c->mutex);
}

bool lru_cache_put(struct lru_cache *c, const char *key, void *data, size_t size)
{
    pthread_mutex_lock(&c->mutex);

    /* Overwrite an existing key: replace the payload, keep the entry. */
    struct lru_entry *old = ht_lookup(&c->table, key);
    if (old) {
        c->current_size -= old->size;
        free(old->data);
        old->data = data;
        old->size = size;
        c->current_size += size;
        list_move_front(c, old);
        pthread_mutex_unlock(&c->mutex);
        return true;
    }

    /* An item larger than the whole cache is never worth caching. */
    if (size > c->max_size) {
        free(data);
        pthread_mutex_unlock(&c->mutex);
        return false;
    }

    struct lru_entry *e = malloc(sizeof(*e));
    if (!e) {
        free(data);
        pthread_mutex_unlock(&c->mutex);
        return false;
    }
    e->key = strdup(key);
    if (!e->key) {
        free(e);
        free(data);
        pthread_mutex_unlock(&c->mutex);
        return false;
    }
    e->data = data;
    e->size = size;
    e->refcount = 0;
    e->removed = false;
    e->prev = NULL;
    e->next = NULL;

    /* Evict least-recently-used entries until both limits are satisfied. */
    while (c->num_entries >= c->max_entries ||
           c->current_size + size > c->max_size) {
        if (!c->tail) {
            break;
        }
        evict_lru(c);
    }

    if (!ht_insert(&c->table, e->key, e)) {
        /* Table could not grow; give back everything the caller owns. */
        free(e->key);
        free(e);
        free(data);
        pthread_mutex_unlock(&c->mutex);
        return false;
    }

    c->num_entries++;
    c->current_size += size;
    list_push_front(c, e);

    pthread_mutex_unlock(&c->mutex);
    return true;
}

size_t lru_cache_size(const struct lru_cache *c)
{
    /* A size_t read of a size_t field is atomic on the platforms we target. */
    return c->current_size;
}

size_t lru_cache_entries(const struct lru_cache *c)
{
    return c->num_entries;
}

void lru_cache_stats(const struct lru_cache *c, unsigned long long *hits,
                     unsigned long long *misses, unsigned long long *evictions)
{
    if (hits) {
        *hits = c->hits;
    }
    if (misses) {
        *misses = c->misses;
    }
    if (evictions) {
        *evictions = c->evictions;
    }
}
