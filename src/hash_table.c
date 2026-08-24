/*
 * hash_table.c - Generic open-addressing hash table.
 *
 * Open addressing with linear probing, tombstones for deletion, and automatic
 * growth (rehash) past a load-factor threshold. This module is pure logic and
 * single-threaded by design — the LRU checkpoint layers locking on top of it.
 */

#include "hash_table.h"

#include <stdlib.h>
#include <string.h>

/* Initial capacity when the caller passes 0. */
#define HT_DEFAULT_CAPACITY 16
/* Grow when (count + tombstones) exceeds this fraction of capacity. */
#define HT_MAX_LOAD_NUMER 7
#define HT_MAX_LOAD_DENOM 10

static size_t next_power_of_two(size_t n)
{
    size_t p = HT_DEFAULT_CAPACITY;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

/*
 * Insert a key/value into the table assuming the caller has already ensured
 * there is headroom (i.e. no resize is needed). Returns false only on a
 * logically-impossible full table (we resize before that can happen).
 */
static bool insert_no_resize(struct hash_table *ht, const void *key, const void *value)
{
    size_t mask = ht->capacity - 1;
    size_t i = ht->hash(key) & mask;
    size_t first_tomb = SIZE_MAX;

    for (size_t n = 0; n < ht->capacity; n++) {
        unsigned char state = ht->slots[i].state;
        if (state == HT_EMPTY) {
            size_t pos = (first_tomb != SIZE_MAX) ? first_tomb : i;
            ht->slots[pos].key = key;
            ht->slots[pos].value = (void *)value;
            ht->slots[pos].state = HT_USED;
            ht->count++;
            return true;
        }
        if (state == HT_TOMBSTONE) {
            if (first_tomb == SIZE_MAX) {
                first_tomb = i;
            }
        } else if (ht->eq(ht->slots[i].key, key)) {
            /* Duplicate key: overwrite the value, keep the original key. */
            ht->slots[i].value = (void *)value;
            return true;
        }
        i = (i + 1) & mask;
    }

    return false;
}

/* Rehash every live entry into a fresh slot array of the given capacity. */
static bool resize(struct hash_table *ht, size_t new_capacity)
{
    struct ht_slot *new_slots = calloc(new_capacity, sizeof(struct ht_slot));
    if (!new_slots) {
        return false;
    }

    struct ht_slot *old_slots = ht->slots;
    size_t old_capacity = ht->capacity;

    ht->slots = new_slots;
    ht->capacity = new_capacity;
    ht->count = 0;
    ht->tombstones = 0;

    for (size_t i = 0; i < old_capacity; i++) {
        if (old_slots[i].state == HT_USED) {
            insert_no_resize(ht, old_slots[i].key, old_slots[i].value);
        }
    }

    free(old_slots);
    return true;
}

/* The slot index holding @key, or SIZE_MAX if it is not present. */
static size_t find_key(const struct hash_table *ht, const void *key)
{
    size_t mask = ht->capacity - 1;
    size_t i = ht->hash(key) & mask;

    for (size_t n = 0; n < ht->capacity; n++) {
        unsigned char state = ht->slots[i].state;
        if (state == HT_EMPTY) {
            return SIZE_MAX; /* empty slot terminates a probe cluster */
        }
        if (state == HT_USED && ht->eq(ht->slots[i].key, key)) {
            return i;
        }
        i = (i + 1) & mask;
    }

    return SIZE_MAX;
}

bool ht_init(struct hash_table *ht, size_t capacity, ht_hash_fn hash, ht_eq_fn eq)
{
    if (!ht || !hash || !eq) {
        return false;
    }

    ht->capacity = next_power_of_two(capacity);
    ht->slots = calloc(ht->capacity, sizeof(struct ht_slot));
    if (!ht->slots) {
        return false;
    }
    ht->count = 0;
    ht->tombstones = 0;
    ht->hash = hash;
    ht->eq = eq;
    return true;
}

struct hash_table *ht_create(size_t capacity, ht_hash_fn hash, ht_eq_fn eq)
{
    struct hash_table *ht = malloc(sizeof(struct hash_table));
    if (!ht) {
        return NULL;
    }
    if (!ht_init(ht, capacity, hash, eq)) {
        free(ht);
        return NULL;
    }
    return ht;
}

void ht_destroy(struct hash_table *ht)
{
    if (!ht) {
        return;
    }
    free(ht->slots);
    ht->slots = NULL;
    ht->capacity = 0;
    ht->count = 0;
    ht->tombstones = 0;
    /* If ht itself was heap-allocated by ht_create(), free it too. */
    /* (We cannot know here; ht_create() callers free the struct separately.) */
}

bool ht_insert(struct hash_table *ht, const void *key, const void *value)
{
    /* Grow before inserting when the load factor (incl. tombstones) is high. */
    if ((ht->count + ht->tombstones + 1) * HT_MAX_LOAD_DENOM >
        ht->capacity * HT_MAX_LOAD_NUMER) {
        if (!resize(ht, ht->capacity * 2)) {
            return false;
        }
    }
    return insert_no_resize(ht, key, value);
}

void *ht_lookup(const struct hash_table *ht, const void *key)
{
    size_t i = find_key(ht, key);
    if (i == SIZE_MAX) {
        return NULL;
    }
    return ht->slots[i].value;
}

bool ht_contains(const struct hash_table *ht, const void *key)
{
    return find_key(ht, key) != SIZE_MAX;
}

bool ht_remove(struct hash_table *ht, const void *key)
{
    size_t i = find_key(ht, key);
    if (i == SIZE_MAX) {
        return false;
    }
    ht->slots[i].state = HT_TOMBSTONE;
    ht->slots[i].key = NULL;
    ht->slots[i].value = NULL;
    ht->count--;
    ht->tombstones++;
    return true;
}

size_t ht_size(const struct hash_table *ht)
{
    return ht->count;
}

size_t ht_capacity(const struct hash_table *ht)
{
    return ht->capacity;
}
