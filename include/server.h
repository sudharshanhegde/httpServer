#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAX_HEADERS 64
#define MAX_PATH 4096 
#define BUFFER_SIZE 8192
#define MAX_EVENTS 1024
#define BACKLOG 1024
#define CACHE_MAX_SIZE 10485760
#define CACHE_MAX_FILES 128
#define MAX_REQUEST_LINE 8192
#define LOG_LINE_SIZE 512
#define CONFIG_LINE_SIZE 256
#define DEFAULT_PORT 8080
#define DEFAULT_THREADS 4
#define ver 16

enum http_method {
    HTTP_GET = 0,
    HTTP_HEAD = 1,
    HTTP_POST = 2,
    HTTP_UNSUPPORTED = 3
};

struct http_request {
    enum http_method method;
    char path[MAX_PATH];
    char query_string[MAX_PATH];
    char version[ver];
    char headers[MAX_HEADERS][256];
    int num_headers;
    bool keep_alive;
    bool parsed;
};

struct http_response {
    int status_code;
    char content_type[64];
    off_t content_length;
    char *body;
    int fd;
    bool from_cache;
    bool use_sendfile;
};

struct server_config {
    char server_root[512];
    int port;
    int num_threads;
    char log_file[512];
    int timeout_secs;
};

struct client_info {
    int fd;
    char client_ip[INET6_ADDRSTRLEN];
    int port;
    char read_buf[BUFFER_SIZE];
    size_t read_len;
    bool keep_alive;
    time_t last_active;
};

struct work_queue {
    struct client_info *jobs;
    int capacity;
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

struct thread_pool {
    pthread_t *threads;
    int num_threads;
    struct work_queue queue;
    volatile bool shutdown;
};

extern struct server_config g_config;
extern struct thread_pool g_pool;
extern struct lru_cache g_cache;
extern volatile bool g_shutdown;
extern FILE *g_log_file;

void log_init(const char *filename);
void log_close(void);
void log_request(const char *client_ip,const char *method, const char *path, int status_code,size_t bytes_sent);
void log_msg(const char *level, const char *fmt, ...);

void config_load(const char *filename, struct server_config *cfg);
void config_print(const struct server_config *cfg);

void thread_pool_init(struct thread_pool *pool, int num_threads);
void thread_pool_destroy(struct thread_pool *pool);
void thread_pool_enqueue(struct thread_pool *pool, struct client_info *client);

/**
 * enum http_parse_status - Result of feeding bytes into an HTTP parser.
 *
 * HTTP_PARSE_INCOMPLETE means more bytes are needed; call http_parser_feed()
 * again with the next chunk. HTTP_PARSE_DONE means the full request
 * (including the terminating blank line) has been parsed into the request
 * struct. HTTP_PARSE_ERROR means the input was malformed and the request must
 * be rejected with a 400; the parser state is unusable afterwards.
 */
enum http_parse_status {
    HTTP_PARSE_INCOMPLETE = 0,
    HTTP_PARSE_DONE = 1,
    HTTP_PARSE_ERROR = -1
};

/**
 * struct http_parser - Incremental HTTP/1.1 request-line + header state machine.
 *
 * Callers own the struct, initialize it with http_parser_init(), then feed it
 * arbitrary byte chunks (including a header split across multiple feeds, or
 * one byte at a time) until it returns HTTP_PARSE_DONE or HTTP_PARSE_ERROR.
 */
struct http_parser {
    struct http_request *req;   /* caller-owned request being filled */
    int state;                  /* internal state-machine state */
    size_t path_pos;            /* write cursor into req->path */
    size_t query_pos;           /* write cursor into req->query_string */
    size_t version_pos;         /* write cursor into req->version */
    char method_tmp[16];        /* scratch buffer for the method token */
    size_t method_len;          /* bytes written into method_tmp */
    size_t name_len;            /* bytes written into the current header name */
    size_t value_pos;           /* offset where a header value starts (after "Name: ") */
    size_t value_len;           /* bytes written into the current header value */
    bool host_seen;             /* a Host header was parsed */
    bool done;                  /* blank line seen; parse finished */
};

/**
 * http_parser_init - Bind a fresh parser to a zeroed request struct.
 *
 * @p:   Uninitialized parser to reset.
 * @req: Caller-owned request struct to fill; should be zeroed by the caller.
 */
void http_parser_init(struct http_parser *p, struct http_request *req);

/**
 * http_parser_feed - Feed a byte chunk into the parser state machine.
 *
 * Feed as much or as little of the request as you have. Returns
 * HTTP_PARSE_INCOMPLETE if more bytes are needed, HTTP_PARSE_DONE once the
 * terminating blank line has been consumed (the request struct is fully
 * populated and req->parsed is set), or HTTP_PARSE_ERROR on malformed input
 * (empty request line, non-token method, oversized field, missing Host on
 * HTTP/1.1, unterminated header block). Once DONE or ERROR is returned the
 * parser is finished; do not feed it again.
 *
 * @p:   Initialized parser from http_parser_init().
 * @buf: Byte buffer containing the next chunk of the request.
 * @len: Number of bytes in @buf.
 *
 * Returns: HTTP_PARSE_DONE, HTTP_PARSE_INCOMPLETE, or HTTP_PARSE_ERROR.
 */
enum http_parse_status http_parser_feed(struct http_parser *p, const char *buf, size_t len);

/**
 * http_parse_request - Parse a buffered client read into a request struct.
 *
 * Convenience wrapper for callers that read once and want a single-shot parse
 * of whatever bytes are available. On success sets req->parsed = true; on an
 * incomplete or malformed read leaves req->parsed = false. This is not a
 * substitute for the incremental http_parser_feed() when a request may arrive
 * across multiple read()s (see the reactor checkpoint).
 *
 * @client: Client whose read_buf/read_len hold the bytes to parse.
 * @req:    Caller-owned request struct to fill (zeroed here).
 */
void http_parse_request(struct client_info *client, struct http_request *req);
const char *http_method_str(enum http_method method);

void file_serve(struct client_info *client, struct http_request *req, struct http_response *resp);
void http_send_response(int client_fd, struct http_response *resp, bool head_only);
const char *mime_type(const char *path);

int server_socket_init(int port);
int server_accept_client(int server_fd, struct client_info *client);
void event_loop(int server_fd);

void signal_handler(int sig);
void setup_signal_handlers(void);

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define HTTP_OK 200
#define HTTP_NOT_MODIFIED 304
#define HTTP_BAD_REQUEST 400
#define HTTP_NOT_FOUND 404
#define HTTP_METHOD_NOT_ALLOWED 405
#define HTTP_REQUEST_TIMEOUT 408
#define HTTP_TOO_LARGE 413
#define HTTP_INTERNAL_ERROR 500
#define HTTP_NOT_IMPLEMENTED 501

#endif /*SERVER_H*/
