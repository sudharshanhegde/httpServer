#include "server.h"

static unsigned int hash_path(const char *path)
{
    unsigned int hash = 5381;
    int c;
    while((c = *path++))
    {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % 1024;
}

static void list_unlink(struct lru_cache *cache, struct cache_node *node)
{
    if(node -> prev)
    {
        node -> prev -> next = node -> next;
    }
    else{
        cache -> head = node -> next;
    }

    if(node -> next)
    {
        node->next->prev = node->prev;
    }
    else{
        cache -> tail = node -> prev;
    }
}

static void list_insert_head(struct lru_cache *cache, struct cache_node *node)
{
    node -> prev = NULL;
    node -> next = cache -> head;
    if(cache->head)
    {
        cache -> head -> prev = node;
    }
    cache -> head = node;
    if(!cache -> tail)
    {
        cache -> tail = node;
    }
}


static void list_move_to_head(struct lru_cache *cache, struct cache_node *node)
{
    if(cache -> head == node)
    {
        return;
    }
    list_unlink(cache,node);
    list_insert_head(cache,node);
}

static void hash_remove(struct lru_cache *cache, struct cache_node * target)
{
    unsigned int bucket = hash_path(target -> path);
    struct cache_node **indirect = &cache -> buckets[bucket];
    while(*indirect) {
        if(*indirect == target)
        {
            *indirect = target -> hash_next;
            return;
        }
        indirect = &(*indirect) -> hash_next;
    }
}

static void evict_lru(struct lru_cache *cache) {
    struct cache_node *victim = cache -> tail;
    if(!victim)
    {
        return;
    }
    hash_remove(cache,victim);
    list_unlink(cache,victim);
    cache -> current_size -= victim -> size;
    cache -> num_entries--;
    free(victim -> data);
    free(victim);
}

void cache_init(struct lru_cache *cache) {
    memset(cache, 0, sizeof(*cache));
    cache -> max_size = CACHE_MAX_SIZE;
    cache -> max_entries = CACHE_MAX_FILES;
    pthread_mutex_init(&cache->mutex, NULL);
    log_msg("INFO", "LRU cache initialised (max %zu bytes, %d files)", cache -> max_size, cache -> max_entries);
}

void cache_destroy(struct lru_cache *cache) {
    pthread_mutex_lock(&cache -> mutex);
    struct cache_node *node = cache -> head;
    while(node) {
        struct cache_node *next = cache -> head;
        free(node -> data);
        free(node);
        node = next;
    }
    pthread_mutex_unlock(&cache -> mutex);
    pthread_mutex_destroy(&cache -> mutex);
    log_msg("INFO", "LRU cache is destroyed.");
}