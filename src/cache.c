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
    
}