/*
 * lru_cache.h - Thread-safe LRU cache built on the open-addressing hash table.
 *
 * The cache maps string keys (request paths) to arbitrary payload blobs. Lookup
 * and insertion are O(1) via the hash table; a doubly-linked list maintains
 * recency order (head = most-recently-used, tail = least-recently-used). When
 * the byte or entry capacity is exceeded, the least-recently-used entry is
 * evicted.
 *
 * Thread-safety: every public function is guarded by a single mutex. To let a
 * caller hold a payload without racing an eviction, lru_cache_get() returns a
 * *borrowed* entry whose reference count has been bumped; the payload stays
 * valid until the caller calls lru_cache_release(). An evicted entry is
 * detached from the cache immediately but its memory is only freed once its
 * reference count reaches zero.
 */

#ifndef LRU_CACHE_H
#define LRU_CACHE_H

#include "hash_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/**
 * struct lru_entry - A cached key/value, and an opaque handle for readers.
 *
 * Returned by lru_cache_get(); the caller reads @data and @size and must call
 * lru_cache_release() when done. Do not inspect the other fields — they are
 * internal bookkeeping.
 */
struct lru_entry {
    void *data;              /* cached payload, owned by the cache */
    size_t size;             /* payload size in bytes */
    char *key;               /* owned copy of the lookup key */
    unsigned refcount;       /* in-use readers; freed when it reaches 0 */
    bool removed;            /* detached from cache, pending refcount-free */
    struct lru_entry *prev;  /* LRU list links (head = MRU, tail = LRU) */
    struct lru_entry *next;
};

/**
 * struct lru_cache - An LRU cache with byte and entry capacity limits.
 *
 * Initialize with lru_cache_init() and tear down with lru_cache_destroy().
 * Treat as opaque outside this module.
 */
struct lru_cache {
    struct hash_table table; /* maps key -> struct lru_entry* */
    struct lru_entry *head;  /* most-recently-used */
    struct lru_entry *tail;  /* least-recently-used */
    size_t current_size;     /* bytes of live (non-removed) entries */
    size_t max_size;         /* byte capacity */
    size_t num_entries;      /* live entries in the LRU list */
    size_t max_entries;      /* entry capacity */
    unsigned long long hits;       /* successful lookups */
    unsigned long long misses;     /* failed lookups */
    unsigned long long evictions;  /* entries evicted to respect capacity */
    pthread_mutex_t mutex;   /* guards all of the above */
};

/**
 * lru_cache_init - Initialize an LRU cache.
 *
 * @c:          Uninitialized cache to initialize.
 * @max_size:   Maximum total payload bytes the cache may hold.
 * @max_entries: Maximum number of entries the cache may hold.
 *
 * Returns: true on success, false on allocation failure.
 */
bool lru_cache_init(struct lru_cache *c, size_t max_size, size_t max_entries);

/**
 * lru_cache_destroy - Free all cached entries and destroy the cache.
 *
 * Must only be called once all outstanding references (from lru_cache_get())
 * have been released. Frees every cached payload and key.
 *
 * @c: Initialized cache to destroy.
 */
void lru_cache_destroy(struct lru_cache *c);

/**
 * lru_cache_get - Look up a key and mark it most-recently-used.
 *
 * On a hit, returns a borrowed entry with its reference count bumped; the
 * caller reads entry->data/entry->size and MUST call lru_cache_release() with
 * the same entry when done. The payload is guaranteed valid until release,
 * even if the entry is evicted in the meantime. On a miss, returns NULL and
 * the caller is expected to load the data and call lru_cache_put().
 *
 * Thread-safe: may be called concurrently from any worker thread.
 *
 * @c:   Initialized cache.
 * @key: Null-terminated lookup key.
 *
 * Returns: borrowed entry on hit, NULL on miss.
 */
struct lru_entry *lru_cache_get(struct lru_cache *c, const char *key);

/**
 * lru_cache_release - Release a borrowed entry from lru_cache_get().
 *
 * Decrements the entry's reference count and frees it if it was already
 * evicted and this was the last reference. Must be called exactly once per
 * successful lru_cache_get(). Passing NULL is a no-op.
 *
 * @c: Initialized cache.
 * @e: Entry returned by lru_cache_get(), or NULL.
 */
void lru_cache_release(struct lru_cache *c, struct lru_entry *e);

/**
 * lru_cache_put - Insert or overwrite a key with a payload.
 *
 * On success the cache takes ownership of @data (it is freed on eviction or
 * destroy), so the caller must not free it afterwards. If @key already exists
 * its payload is replaced and the old one freed. If @size exceeds @max_size
 * the payload is not cached (data is freed) and false is returned. Evicts
 * least-recently-used entries as needed to respect the capacity limits.
 *
 * Thread-safe: may be called concurrently from any worker thread.
 *
 * @c:    Initialized cache.
 * @key:  Null-terminated lookup key (copied by the cache).
 * @data: Payload buffer; the cache takes ownership on success.
 * @size: Number of bytes in @data.
 *
 * Returns: true if the entry was cached, false on failure (data freed).
 */
bool lru_cache_put(struct lru_cache *c, const char *key, void *data, size_t size);

/**
 * lru_cache_size - Total payload bytes currently held (live entries).
 *
 * @c: Initialized cache.
 *
 * Returns: the current byte usage.
 */
size_t lru_cache_size(const struct lru_cache *c);

/**
 * lru_cache_entries - Number of live entries currently held.
 *
 * @c: Initialized cache.
 *
 * Returns: the current entry count.
 */
size_t lru_cache_entries(const struct lru_cache *c);

/**
 * lru_cache_stats - Snapshot the cache hit/miss/eviction counters.
 *
 * @c:       Initialized cache.
 * @hits:    Optional out-parameter for the hit count (may be NULL).
 * @misses:  Optional out-parameter for the miss count (may be NULL).
 * @evictions: Optional out-parameter for the eviction count (may be NULL).
 */
void lru_cache_stats(const struct lru_cache *c, unsigned long long *hits,
                     unsigned long long *misses, unsigned long long *evictions);

#endif /* LRU_CACHE_H */
