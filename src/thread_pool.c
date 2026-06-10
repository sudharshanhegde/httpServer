#include "server.h"

static void *worker_thread_func(void *arg);

void thread_pool_init(struct thread_pool *pool, int num_threads) {
    int i = 0;
    pool -> num_threads = num_threads;
    pool ->shutdown = false;
    pool -> queue.capacity = num_threads * 2;
    pool ->queue.jobs = malloc(sizeof(struct client_info) * pool -> queue.capacity);
    pool ->queue.head = 0;
    pool -> queue.tail = 0;
    pool -> queue.count = 0;
    pthread_mutex_init(&pool -> queue.mutex, NULL);
    pthread_cond_init(&pool->queue.cond, NULL);
    pool -> threads = malloc(sizeof(pthread_t) * num_threads);
    for(i = 0; i < num_threads; i++)
    {
        if(pthread_create(&pool->threads[i], NULL, worker_thread_func, pool))
        {
            perror("Could not create the worker thread, Failing...")
            exit(EXIT_FAILURE);
        }
    }
    log_msg("INFO", "Thread pool has been initialised with %d threads", num_threads);
}

void thread_pool_destroy(struct thread_pool *pool) {
    int i = 0;
    pool -> shutdown = true;
    pthread_cond_broadcast(&pool -> queue.cond);
    for(i = 0;i < pool -> num_threads; i++)
    {
        pthread_join(pool->threads[i], NULL);
    }
    free(pool->threads);
    free(pool->queue.jobs);
    pthread_mutex_destroy(&pool->queue.mutex);
    pthread_cond_destroy(&pool->queue.cond);
    log_msg("INFO", "Thread pool has been destoryed succesfully");
}

void thread_pool_enqueue(struct thread_pool *pool, struct client_info *client) {
    pthread_mutex_lock(&pool -> queue.mutex);
    if(pool -> queue.count >= pool -> queue.capacity){
        int new_capacity = pool -> queue.capacity * 2;
        struct client_info *new_jobs = realloc(pool -> queue.jobs, sizeof(struct client_info) * new_capacity);
        if(!new_jobs)
        {
            log_msg("ERROR", "We are unable to resize work queue");
            pthread_mutex_unlock(&pool -> queue.mutex);
            close(client->fd);
            return;
        }
        pool -> queue.jobs = new_jobs;
        if(pool -> queue.tail < pool -> queue.head)
        {
            memcpy(&new_jobs[pool->queue.capacity], new_jobs, sizeof(client_info) *pool->queue.head);
            pool -> queue.head += pool-> queue.capacity;
        }
        pool -> queue.capacity = new_capacity;
        log_msg("DEBUG", "Work queue has been resized to %d", new_capacity);
        pool -> queue.jobs[pool->queue.tail] = *client;
        pool -> queue.tail = (pool->queue.tail + 1) % pool -> queue.capacity;
        pool->queue.count++;
        pthread_cond_signal(&pool->queue.cond);
        pthread_mutex_unlock(&pool->queue.mutex);

    }
    
}