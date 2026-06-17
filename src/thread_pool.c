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
            perror("Could not create the worker thread, Failing...");
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
        int old_capacity = pool -> queue.capacity;
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
            memcpy(&new_jobs[pool->queue.capacity], new_jobs, sizeof(struct client_info) *pool->queue.head);
            pool -> queue.head += old_capacity;
            pool -> queue.tail += old_capacity;
            pool ->queue.capacity = new_capacity;
        }
        pool -> queue.capacity = new_capacity;
        log_msg("DEBUG", "Work queue has been resized to %d", new_capacity);
        pool -> queue.jobs[pool->queue.tail] = *client;
        pool -> queue.tail = (pool->queue.tail + 1) % pool -> queue.capacity;
        pool->queue.count++;
        pthread_cond_signal(&pool->queue.cond);
        pthread_mutex_unlock(&pool->queue.mutex);

    }
    else{
        pool -> queue.jobs[pool -> queue.tail] = *client;
        pool -> queue.tail = (pool -> queue.tail + 1) % pool->queue.capacity;
        pool -> queue.count++;
        pthread_cond_signal(&pool -> queue.cond);
    }
    pthread_mutex_unlock(&pool->queue.mutex);
}

static void *worker_thread_func(void *arg){
    struct thread_pool *pool = (struct thread_pool*)arg;
    while(true)
    {
        struct client_info client;
        pthread_mutex_lock(&pool -> queue.mutex);
        while (pool->queue.count == 0 && !pool -> shutdown)
        {
            pthread_cond_wait(&pool -> queue.cond,&pool->queue.mutex);
        }
        if(pool ->shutdown && pool -> queue.count == 0)
        {
            pthread_mutex_unlock(&pool -> queue.mutex);
            break;
        }
        client = pool -> queue.jobs[pool -> queue.head];
        pool -> queue.head = (pool -> queue.head + 1) % pool -> queue.capacity;
        pool -> queue.count--;
        pthread_mutex_unlock(&pool -> queue.mutex);
        ssize_t bytes_read = read(client.fd, client.read_buf, sizeof(client.read_buf) - 1);
        if(bytes_read <= 0)
        {
            if(bytes_read < 0)
            {
                log_msg("WARN","read is failing for %s for the reason %s",client.client_ip,strerror(errno));
            }
            close(client.fd);
            continue;
        }
        client.read_buf[bytes_read] = '\0';
        client.read_len = (size_t)bytes_read;
        struct http_request req;
        memset(&req,0,sizeof(req));
        http_parse_request(&client,&req);
        log_msg("INFO", "%s %s %s from %s",http_method_str(req.method),req.path,req.version,client.client_ip);
        struct http_response resp;
        memset(&resp, 0, sizeof(resp));
        if(req.method == HTTP_GET || req.method == HTTP_HEAD)
        {
            file_serve(&client, &req, &resp);
        }
        else{
            resp.status_code = HTTP_METHOD_NOT_ALLOWED;
            resp.content_length = 0;
            resp.fd = -1;
            resp.use_sendfile = false;
        }
        http_send_response(client.fd, &resp, (req.method == HTTP_HEAD));
        log_request(client.client_ip,http_method_str(req.method), req.path, resp.status_code,(size_t)(resp.content_length > 0 ? resp.content_length : 0));
        if(resp.fd >= 0)
        {
            close(resp.fd);
        }
        shutdown(client.fd,SHUT_WR);
        close(client.fd);
    }
    return NULL;
}