/*
 * test_hash_table.c - End-to-end tests for the generic
 * open-addressing hash table (src/hash_table.c).
 *
 * The table is generic and thread-agnostic, so these tests are deterministic
 * single-threaded unit tests covering insert/lookup/delete, overwrite,
 * collision handling, resize/rehash, tombstone behavior, and a randomized
 * stress pass. Each named test localizes a single class of failure.
 */

#include "hash_table.h"

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

/* ---- string keys -------------------------------------------------------- */

static uint64_t str_hash(const void *key)
{
    const unsigned char *s = key;
    uint64_t h = 1469598103934665603ULL; /* FNV-1a 64-bit offset basis */
    while (*s) {
        h ^= *s++;
        h *= 1099511628211ULL; /* FNV-1a 64-bit prime */
    }
    return h;
}

static bool str_eq(const void *a, const void *b)
{
    return strcmp(a, b) == 0;
}

/* A deliberately terrible hash that maps everything to the same bucket. */
static uint64_t degenerate_hash(const void *key)
{
    (void)key;
    return 0;
}

/* ---- tests -------------------------------------------------------------- */

static void test_insert_and_lookup(void)
{
    struct hash_table ht;
    CHECK(ht_init(&ht, 0, str_hash, str_eq));

    CHECK(ht_insert(&ht, "apple", (void *)1) == true);
    CHECK(ht_insert(&ht, "banana", (void *)2) == true);
    CHECK(ht_insert(&ht, "cherry", (void *)3) == true);

    CHECK(ht_size(&ht) == 3);
    CHECK(ht_lookup(&ht, "apple") == (void *)1);
    CHECK(ht_lookup(&ht, "banana") == (void *)2);
    CHECK(ht_lookup(&ht, "cherry") == (void *)3);
    CHECK(ht_lookup(&ht, "missing") == NULL);
    CHECK(ht_contains(&ht, "apple") == true);
    CHECK(ht_contains(&ht, "missing") == false);

    ht_destroy(&ht);
}

static void test_overwrite_duplicate_key(void)
{
    struct hash_table ht;
    CHECK(ht_init(&ht, 0, str_hash, str_eq));

    CHECK(ht_insert(&ht, "apple", (void *)1) == true);
    CHECK(ht_insert(&ht, "apple", (void *)99) == true);

    CHECK(ht_size(&ht) == 1); /* overwrite does not grow the table */
    CHECK(ht_lookup(&ht, "apple") == (void *)99);

    ht_destroy(&ht);
}

static void test_null_value_disambiguation(void)
{
    struct hash_table ht;
    CHECK(ht_init(&ht, 0, str_hash, str_eq));

    CHECK(ht_insert(&ht, "x", NULL) == true);
    CHECK(ht_contains(&ht, "x") == true);   /* present even though value is NULL */
    CHECK(ht_lookup(&ht, "x") == NULL);     /* but lookup() returns NULL */
    CHECK(ht_contains(&ht, "y") == false);

    ht_destroy(&ht);
}

static void test_remove(void)
{
    struct hash_table ht;
    CHECK(ht_init(&ht, 0, str_hash, str_eq));

    CHECK(ht_insert(&ht, "one", (void *)1) == true);
    CHECK(ht_insert(&ht, "two", (void *)2) == true);
    CHECK(ht_insert(&ht, "three", (void *)3) == true);

    CHECK(ht_remove(&ht, "two") == true);
    CHECK(ht_remove(&ht, "two") == false);      /* already gone */
    CHECK(ht_size(&ht) == 2);
    CHECK(ht_contains(&ht, "two") == false);
    CHECK(ht_lookup(&ht, "one") == (void *)1);
    CHECK(ht_lookup(&ht, "three") == (void *)3);

    /* Reinsert after a tombstone works and restores the count. */
    CHECK(ht_insert(&ht, "two", (void *)22) == true);
    CHECK(ht_size(&ht) == 3);
    CHECK(ht_lookup(&ht, "two") == (void *)22);

    ht_destroy(&ht);
}

static void test_collision_handling(void)
{
    /* All keys hash to bucket 0; linear probing must resolve them. */
    struct hash_table ht;
    CHECK(ht_init(&ht, 0, degenerate_hash, str_eq));

    const char *keys[] = { "a", "b", "c", "d", "e", "f", "g", "h" };
    int n = (int)(sizeof(keys) / sizeof(keys[0]));
    for (int i = 0; i < n; i++) {
        CHECK(ht_insert(&ht, keys[i], (void *)(intptr_t)(i + 1)) == true);
    }
    CHECK(ht_size(&ht) == (size_t)n);

    for (int i = 0; i < n; i++) {
        CHECK(ht_lookup(&ht, keys[i]) == (void *)(intptr_t)(i + 1));
    }

    /* Remove a middle element; neighbors must still be findable. */
    CHECK(ht_remove(&ht, "d") == true);
    CHECK(ht_contains(&ht, "d") == false);
    CHECK(ht_lookup(&ht, "c") == (void *)3);
    CHECK(ht_lookup(&ht, "e") == (void *)5);

    ht_destroy(&ht);
}

static void test_resize_and_rehash(void)
{
    enum { NKEYS = 2000 };
    struct hash_table ht;

    CHECK(ht_init(&ht, 16, str_hash, str_eq));
    size_t cap_before = ht_capacity(&ht);
    CHECK(cap_before >= 16);

    /*
     * The table stores key pointers by value (it does not copy them), so the
     * keys must outlive their entries. Allocate persistent storage rather
     * than a stack buffer that would go out of scope each iteration.
     */
    char (*keys)[32] = malloc(sizeof(char[32]) * NKEYS);
    CHECK(keys != NULL);

    /* Insert far more than the initial capacity to force multiple resizes. */
    for (int i = 0; i < NKEYS; i++) {
        snprintf(keys[i], 32, "key-%04d", i);
        CHECK(ht_insert(&ht, keys[i], (void *)(intptr_t)(i + 1000)) == true);
    }

    CHECK(ht_size(&ht) == (size_t)NKEYS);
    CHECK(ht_capacity(&ht) > cap_before);

    /* Every key must survive the rehashes intact. */
    for (int i = 0; i < NKEYS; i++) {
        CHECK(ht_lookup(&ht, keys[i]) == (void *)(intptr_t)(i + 1000));
    }

    /* Remove half; the survivors must still resolve. */
    for (int i = 0; i < NKEYS; i += 2) {
        CHECK(ht_remove(&ht, keys[i]) == true);
    }
    CHECK(ht_size(&ht) == (size_t)(NKEYS / 2));
    for (int i = 1; i < NKEYS; i += 2) {
        CHECK(ht_lookup(&ht, keys[i]) == (void *)(intptr_t)(i + 1000));
    }

    free(keys);
    ht_destroy(&ht);
}

static void test_random_key_stress(void)
{
    enum { NKEYS = 5000 };
    struct hash_table ht;
    CHECK(ht_init(&ht, 0, str_hash, str_eq));

    /* Deterministic LCG so the test is reproducible. */
    uint32_t state = 0x12345678u;
    char (*keys)[16] = malloc(sizeof(char[16]) * NKEYS);
    CHECK(keys != NULL);
    int *values = malloc(sizeof(int) * NKEYS);
    CHECK(values != NULL);

    for (int i = 0; i < NKEYS; i++) {
        state = state * 1664525u + 1013904223u;
        snprintf(keys[i], 16, "k%08x", state);
        values[i] = (int)(state % 100000);
        CHECK(ht_insert(&ht, keys[i], &values[i]) == true);
    }
    CHECK(ht_size(&ht) == (size_t)NKEYS);

    for (int i = 0; i < NKEYS; i++) {
        CHECK(ht_lookup(&ht, keys[i]) == &values[i]);
    }

    /* Remove every third key (i % 3 == 0). */
    size_t removed = (size_t)((NKEYS + 2) / 3);
    for (int i = 0; i < NKEYS; i += 3) {
        CHECK(ht_remove(&ht, keys[i]) == true);
    }
    CHECK(ht_size(&ht) == (size_t)NKEYS - removed);

    for (int i = 0; i < NKEYS; i++) {
        if (i % 3 == 0) {
            CHECK(ht_contains(&ht, keys[i]) == false);
        } else {
            CHECK(ht_lookup(&ht, keys[i]) == &values[i]);
        }
    }

    free(keys);
    free(values);
    ht_destroy(&ht);
}

static void test_ht_create_and_capacity(void)
{
    struct hash_table *ht = ht_create(0, str_hash, str_eq);
    CHECK(ht != NULL);
    CHECK(ht_capacity(ht) > 0);
    CHECK(ht_insert(ht, "a", (void *)1) == true);
    CHECK(ht_lookup(ht, "a") == (void *)1);
    ht_destroy(ht);
    free(ht);
}

int main(void)
{
    test_insert_and_lookup();
    test_overwrite_duplicate_key();
    test_null_value_disambiguation();
    test_remove();
    test_collision_handling();
    test_resize_and_rehash();
    test_random_key_stress();
    test_ht_create_and_capacity();

    if (g_failures == 0) {
        printf("test_hash_table: all tests passed\n");
        return 0;
    }
    fprintf(stderr, "test_hash_table: %d test(s) FAILED\n", g_failures);
    return 1;
}
