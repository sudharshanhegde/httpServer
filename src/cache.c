/*
 * cache.c - Superseded by src/lru_cache.c (Checkpoint 3).
 *
 * The original hand-written cache used a chained-bucket hash table and was
 * untested. The skill's checkpointed workflow replaced it with a thread-safe
 * LRU cache built on the generic open-addressing hash table. See lru_cache.h
 * and lru_cache.c. This file is retained only so the `wildcard` in the
 * Makefile does not pick up an orphaned object; it contains no code.
 */
