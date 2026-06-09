#include "server.h"

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_init(const char *filename) {
    g_log_file = fopen(filename,"a");
    if(!g_log_file) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }
    log_msg("INFO", "Server is Starting...");
}

void log_close(void) {
    if(g_log_file){
        log_msg("INFO","Server is shutting down");
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void log_request(const char *client_ip, const char *method, const char *path, int status_code, size_t bytes_sent){
    struct timespec ts;
    struct tm tm_result;
    char timebuf[64];
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm_result);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_result);
    pthread_mutex_lock(&g_log_mutex);
    fprintf(g_log_file,"[%s] %d %s %s %zu bytes from %s\n", timebuf, status_code, method, path, bytes_sent, client_ip);
    fflush(g_log_file);
    pthread_mutex_unlock(&g_log_mutex);
}

void log_msg(const char *level, const char *fmt, ...){
    struct timespec ts;
    struct tm tm_result;
    char timebuf[64];
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm_result);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_result);
    va_start(args,fmt);
    vfprintf(g_log_file, fmt, args);
    va_end(args);
    fputc('\n', g_log_file);
    fflush(g_log_file);
    pthread_mutex_unlock(&g_log_mutex);
}