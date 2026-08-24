/*
 * hash_table.h - Generic open-addressing hash table.
 *
 * A reusable, thread-agnostic key/value map with no knowledge of what the keys
 * or values are: the caller supplies a hash function and an equality function,
 * and the table stores raw void* keys and values (which it never frees — the
 * caller owns them). It is the storage layer the LRU cache (Checkpoint 3) is
 * built on, but is deliberately generic so it can be unit-tested in isolation.
 *
 * Open addressing with linear probing; deleted slots become tombstones; the
 * table grows (rehashing) once the load factor climbs past a threshold.
 */

#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Hash a key to a 64-bit digest; only the low bits are used as an index. */
typedef uint64_t (*ht_hash_fn)(const void *key);

/** Compare two keys for equality; returns true if they are the same key. */
typedef bool (*ht_eq_fn)(const void *a, const void *b);

/* Internal slot states. */
enum ht_slot_state {
    HT_EMPTY = 0,      /* never used */
    HT_USED = 1,       /* holds a live key/value */
    HT_TOMBSTONE = 2   /* deleted; reclaimed on resize */
};

struct ht_slot {
    const void *key;
    void *value;
    unsigned char state;
};

/**
 * struct hash_table - An open-addressing key/value map.
 *
 * Callers must initialize with ht_init() (or ht_create()) and destroy with
 * ht_destroy(). All fields except @slots are bookkeeping; treat the struct as
 * opaque outside this module. Not thread-safe: the LRU checkpoint layers a
 * lock on top.
 */
struct hash_table {
    struct ht_slot *slots;
    size_t capacity;    /* number of slots, always a power of two */
    size_t count;       /* number of live entries */
    size_t tombstones;  /* number of deleted slots not yet reclaimed */
    ht_hash_fn hash;    /* caller-supplied key hash */
    ht_eq_fn eq;        /* caller-supplied key equality */
};

/**
 * ht_init - Initialize a hash table with a given initial capacity.
 *
 * @ht:       Uninitialized table to initialize.
 * @capacity: Requested minimum capacity (rounded up to a power of two; 0 uses
 *            a sane default). The table grows automatically beyond this.
 * @hash:     Caller-supplied key hash function (must not be NULL).
 * @eq:       Caller-supplied key equality function (must not be NULL).
 *
 * Returns: true on success, false on allocation failure.
 */
bool ht_init(struct hash_table *ht, size_t capacity, ht_hash_fn hash, ht_eq_fn eq);

/**
 * ht_create - Allocate and initialize a hash table on the heap.
 *
 * Convenience wrapper around ht_init() for callers that prefer heap
 * allocation. Returns a table that must be freed with ht_destroy() (which
 * frees both the table and its slots).
 *
 * @capacity: Requested minimum capacity (rounded up to a power of two).
 * @hash:     Caller-supplied key hash function.
 * @eq:       Caller-supplied key equality function.
 *
 * Returns: a heap-allocated table, or NULL on allocation failure.
 */
struct hash_table *ht_create(size_t capacity, ht_hash_fn hash, ht_eq_fn eq);

/**
 * ht_destroy - Free a hash table's slot array.
 *
 * Does NOT free the keys or values stored in the table — the caller owns
 * those. Safe to call on a table initialized with ht_init(). If @ht was
 * allocated with ht_create(), the caller must additionally free() the struct
 * after calling ht_destroy().
 *
 * @ht: Table to destroy. May be NULL.
 */
void ht_destroy(struct hash_table *ht);

/**
 * ht_insert - Insert or overwrite a key/value pair.
 *
 * If @key is already present (per @eq), its value is replaced with @value and
 * the existing entry's key pointer is left unchanged. Otherwise a new entry
 * is stored. The table stores the key/value pointers by value; it does not
 * copy them, so they must remain valid for the lifetime of the entry.
 *
 * @ht:    Initialized table.
 * @key:   Caller-owned key pointer.
 * @value: Caller-owned value pointer (may be NULL).
 *
 * Returns: true on success, false on allocation failure (table unchanged).
 */
bool ht_insert(struct hash_table *ht, const void *key, const void *value);

/**
 * ht_lookup - Return the value for a key, or NULL if the key is absent.
 *
 * @ht:  Initialized table.
 * @key: Key to search for.
 *
 * Returns: the stored value pointer, or NULL if the key is not present. Note
 * this is ambiguous with a stored NULL value; use ht_contains() to
 * disambiguate.
 */
void *ht_lookup(const struct hash_table *ht, const void *key);

/**
 * ht_contains - Report whether a key is present.
 *
 * @ht:  Initialized table.
 * @key: Key to search for.
 *
 * Returns: true if present, false otherwise.
 */
bool ht_contains(const struct hash_table *ht, const void *key);

/**
 * ht_remove - Remove a key from the table.
 *
 * Leaves a tombstone in place (reclaimed on the next resize). Does not free
 * the key or value.
 *
 * @ht:  Initialized table.
 * @key: Key to remove.
 *
 * Returns: true if the key was present and removed, false if it was absent.
 */
bool ht_remove(struct hash_table *ht, const void *key);

/**
 * ht_size - Number of live entries currently stored.
 *
 * @ht: Initialized table.
 *
 * Returns: the number of live entries.
 */
size_t ht_size(const struct hash_table *ht);

/**
 * ht_capacity - Number of slots in the table (always a power of two).
 *
 * @ht: Initialized table.
 *
 * Returns: the current slot count.
 */
size_t ht_capacity(const struct hash_table *ht);

#endif /* HASH_TABLE_H */
