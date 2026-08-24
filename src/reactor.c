/*
 * reactor.c - Single-threaded, edge-triggered epoll HTTP reactor.
 *
 * Edge-triggered (EPOLLET) semantics: after an EPOLLIN event we read until
 * EAGAIN (fully drain the socket), and after an EPOLLOUT event we write until
 * EAGAIN. A request may arrive across many read()s; the incremental parser is
 * fed one byte at a time so that on completion we know exactly how many bytes
 * belong to the request and can carry any pipelined
 * remainder into the next request on a keep-alive connection. Partial writes
 * are tracked per connection (write offset) and the socket is re-armed for
 * EPOLLOUT until the whole response is flushed.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reactor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Maximum accepted socket fd number; fds above this are refused. */
#define CONN_MAX 65536

/* Per-connection lifecycle state. */
enum conn_state {
    ST_READING = 0, /* awaiting a full request */
    ST_WRITING,     /* flushing a response, tracking write_off */
    ST_CLOSED
};

/* Response transmission phases. */
enum resp_phase {
    RSP_HEAD = 0,    /* flushing the response head (headers, maybe inline body) */
    RSP_SENDFILE,    /* zero-copy sendfile() of the file body */
    RSP_DONE         /* nothing left to send */
};

struct conn {
    int fd;
    enum conn_state state;
    bool keep_alive;

    /* Input: unconsumed request bytes in inbuf[in_head..in_tail). */
    char inbuf[BUFFER_SIZE];
    size_t in_head;
    size_t in_tail;

    /* Incremental request parser. */
    struct http_parser parser;
    struct http_request req;

    /* Output: response head buffer (headers, plus inline body for simple
     * responses), then an optional file body streamed via sendfile(). */
    enum resp_phase phase;
    char *head;          /* malloc'd head block; freed once fully written */
    size_t head_len;
    size_t head_off;     /* bytes of head already written */
    int file_fd;         /* file to sendfile(), or -1 */
    off_t file_off;      /* current sendfile offset */
    off_t file_len;      /* bytes remaining in the file */
};

struct reactor {
    int epfd;
    int listen_fd;
    int wakeup_fd;
    int port;
    char *doc_root;
    int max_events;
    int active_conns;    /* atomic: live connections (read by the pool tuner) */
    struct conn **conns; /* indexed by fd */
    volatile bool stop;
};

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void mod_events(struct reactor *r, struct conn *c, uint32_t events)
{
    struct epoll_event ev;
    ev.events = events | EPOLLET;
    ev.data.fd = c->fd;
    epoll_ctl(r->epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

static void conn_close(struct reactor *r, struct conn *c)
{
    epoll_ctl(r->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    if (c->head) {
        free(c->head);
    }
    if (c->file_fd >= 0) {
        close(c->file_fd);
    }
    r->conns[c->fd] = NULL;
    free(c);
    __atomic_sub_fetch(&r->active_conns, 1, __ATOMIC_RELAXED);
}

/* ---- response building (sendfile() zero-copy data plane) ----------------- */

static const char *status_reason(int code)
{
    switch (code) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 413: return "413 Payload Too Large";
    case 500: return "500 Internal Server Error";
    default:  return "500 Internal Server Error";
    }
}

static const char *reactor_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
        return "text/html";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript";
    }
    if (strcmp(ext, ".txt") == 0) {
        return "text/plain";
    }
    if (strcmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(ext, ".gif") == 0) {
        return "image/gif";
    }
    if (strcmp(ext, ".ico") == 0) {
        return "image/x-icon";
    }
    return "application/octet-stream";
}

/* Build just the response head; caller appends the body. */
static char *build_head(int code, const char *ctype, size_t content_length,
                        bool keep_alive, size_t *out_len)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     status_reason(code), ctype, content_length,
                     keep_alive ? "keep-alive" : "close");
    char *buf = malloc((size_t)n);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, head, (size_t)n);
    *out_len = (size_t)n;
    return buf;
}

/* Set a simple status-only response (e.g. an error page), sent entirely from
 * the head buffer. */
static void respond_simple(struct conn *c, int code, const char *ctype, const char *body)
{
    size_t blen = strlen(body);
    size_t hlen;
    char *head = build_head(code, ctype, blen, c->keep_alive, &hlen);
    if (!head) {
        return;
    }
    char *buf = malloc(hlen + blen);
    if (!buf) {
        free(head);
        return;
    }
    memcpy(buf, head, hlen);
    free(head);
    memcpy(buf + hlen, body, blen);
    c->head = buf;
    c->head_len = hlen + blen;
    c->head_off = 0;
    c->file_fd = -1;
    c->phase = RSP_HEAD;
}

/*
 * Build a file response (or an error if the path is invalid). The response
 * head is buffered in memory; the body is served zero-copy with sendfile()
 * directly from the file descriptor to the socket.
 */
static void respond_file(struct conn *c, const char *doc_root, const char *path)
{
    /* Reject path traversal outright. */
    if (strstr(path, "..") != NULL) {
        respond_simple(c, 403, "text/plain", "403 Forbidden\n");
        return;
    }

    const char *rel = path;
    if (strcmp(path, "/") == 0) {
        rel = "/index.html";
    }

    char full[MAX_PATH + 64];
    snprintf(full, sizeof(full), "%s%s", doc_root, rel);

    struct stat st;
    if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
        respond_simple(c, HTTP_NOT_FOUND, "text/plain", "404 Not Found\n");
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        respond_simple(c, 403, "text/plain", "403 Forbidden\n");
        return;
    }

    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        respond_simple(c, HTTP_INTERNAL_ERROR, "text/plain", "500 Internal Server Error\n");
        return;
    }

    const char *ctype = reactor_mime_type(rel);
    bool head_only = (c->req.method == HTTP_HEAD);
    size_t hlen;
    char *head = build_head(HTTP_OK, ctype, (size_t)st.st_size, c->keep_alive, &hlen);
    if (!head) {
        close(fd);
        respond_simple(c, HTTP_INTERNAL_ERROR, "text/plain", "500 Internal Server Error\n");
        return;
    }

    c->head = head;
    c->head_len = hlen;
    c->head_off = 0;
    c->phase = RSP_HEAD;
    if (head_only) {
        c->file_fd = -1;
        close(fd);
    } else {
        c->file_fd = fd;
        c->file_off = 0;
        c->file_len = st.st_size;
    }
}

static void build_response(struct reactor *r, struct conn *c)
{
    c->keep_alive = c->req.keep_alive;

    if (c->req.method == HTTP_GET || c->req.method == HTTP_HEAD) {
        respond_file(c, r->doc_root, c->req.path);
    } else {
        respond_simple(c, HTTP_METHOD_NOT_ALLOWED, "text/plain", "405 Method Not Allowed\n");
    }
}

static void build_error_response(struct conn *c, int code)
{
    /* A malformed request is never kept alive. */
    c->keep_alive = false;
    char msg[64];
    snprintf(msg, sizeof(msg), "%d %s\n", code, status_reason(code));
    respond_simple(c, code, "text/plain", msg);
}

/* ---- input processing ---------------------------------------------------- */

static void conn_process_input(struct reactor *r, struct conn *c)
{
    while (c->in_head < c->in_tail && c->state == ST_READING) {
        enum http_parse_status st =
            http_parser_feed(&c->parser, &c->inbuf[c->in_head], 1);
        c->in_head++;
        if (st == HTTP_PARSE_DONE) {
            build_response(r, c);
            c->state = ST_WRITING;
            return;
        }
        if (st == HTTP_PARSE_ERROR) {
            build_error_response(c, HTTP_BAD_REQUEST);
            c->state = ST_WRITING;
            return;
        }
        /* INCOMPLETE: keep consuming bytes. */
    }
}

static void conn_read(struct reactor *r, struct conn *c)
{
    for (;;) {
        /* Compact when fully consumed so the buffer is reusable. */
        if (c->in_head == c->in_tail) {
            c->in_head = c->in_tail = 0;
        }
        if (c->in_tail == BUFFER_SIZE) {
            build_error_response(c, HTTP_TOO_LARGE);
            c->state = ST_WRITING;
            return;
        }

        ssize_t n = read(c->fd, c->inbuf + c->in_tail, BUFFER_SIZE - c->in_tail);
        if (n > 0) {
            c->in_tail += (size_t)n;
            conn_process_input(r, c);
            if (c->state == ST_WRITING) {
                return; /* response ready; stop reading this burst */
            }
            /* INCOMPLETE: keep draining in the same burst (edge-triggered). */
        } else if (n == 0) {
            conn_close(r, c); /* client closed (EOF) */
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return; /* socket drained — that's the end of the edge */
            }
            if (errno == EINTR) {
                continue;
            }
            conn_close(r, c);
            return;
        }
    }
}

/* ---- output processing --------------------------------------------------- */

static void conn_write(struct reactor *r, struct conn *c)
{
    /* Phase 1: flush the response head (headers, plus any inline body). */
    if (c->phase == RSP_HEAD) {
        while (c->head_off < c->head_len) {
            ssize_t n = write(c->fd, c->head + c->head_off, c->head_len - c->head_off);
            if (n > 0) {
                c->head_off += (size_t)n;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                mod_events(r, c, EPOLLOUT); /* wait for writability, resume later */
                return;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                conn_close(r, c);
                return;
            }
        }
        free(c->head);
        c->head = NULL;
        c->head_off = c->head_len = 0;
        c->phase = (c->file_fd >= 0) ? RSP_SENDFILE : RSP_DONE;
    }

    /* Phase 2: sendfile() the file body directly to the socket. */
    if (c->phase == RSP_SENDFILE) {
        while (c->file_off < c->file_len) {
            off_t remaining = c->file_len - c->file_off;
            size_t count = (size_t)remaining;
            if (count > (size_t)0x7ffff000) { /* keep each call modest */
                count = (size_t)0x7ffff000;
            }
            ssize_t n = sendfile(c->fd, c->file_fd, &c->file_off, count);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    mod_events(r, c, EPOLLOUT);
                    return;
                }
                if (errno == EINTR) {
                    continue;
                }
                close(c->file_fd);
                c->file_fd = -1;
                conn_close(r, c);
                return;
            }
            /* n > 0: sendfile advanced c->file_off; loop to send the rest. */
        }
        close(c->file_fd);
        c->file_fd = -1;
        c->phase = RSP_DONE;
    }

    if (c->phase != RSP_DONE) {
        return;
    }

    if (!c->keep_alive) {
        conn_close(r, c);
        return;
    }

    /* Persistent connection: reset and service any pipelined bytes. */
    c->state = ST_READING;
    http_parser_init(&c->parser, &c->req);
    conn_process_input(r, c);
    if (c->state == ST_WRITING) {
        c->phase = RSP_HEAD;
        c->head_off = 0;
        conn_write(r, c); /* try to flush the next response immediately */
        return;
    }
    mod_events(r, c, EPOLLIN);
}

/* ---- accept -------------------------------------------------------------- */

static void reactor_accept(struct reactor *r)
{
    for (;;) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(r->listen_fd, (struct sockaddr *)&addr, &alen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
                return; /* no more pending connections in this edge */
            }
            return;
        }
        if (fd >= CONN_MAX) {
            close(fd);
            continue;
        }
        set_nonblocking(fd);

        struct conn *c = calloc(1, sizeof(*c));
        if (!c) {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->state = ST_READING;
        http_parser_init(&c->parser, &c->req);
        r->conns[fd] = c;
        __atomic_add_fetch(&r->active_conns, 1, __ATOMIC_RELAXED);

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        epoll_ctl(r->epfd, EPOLL_CTL_ADD, fd, &ev);
    }
}

/* ---- public API ---------------------------------------------------------- */

struct reactor *reactor_create(const struct reactor_config *cfg)
{
    if (!cfg) {
        return NULL;
    }

    struct reactor *r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->conns = calloc(CONN_MAX, sizeof(struct conn *));
    if (!r->conns) {
        free(r);
        return NULL;
    }
    r->doc_root = strdup(cfg->doc_root ? cfg->doc_root : "www");
    if (!r->doc_root) {
        free(r->conns);
        free(r);
        return NULL;
    }
    r->max_events = cfg->max_events > 0 ? cfg->max_events : MAX_EVENTS;
    int backlog = cfg->backlog > 0 ? cfg->backlog : BACKLOG;

    r->epfd = epoll_create1(0);
    if (r->epfd < 0) {
        free(r->doc_root);
        free(r->conns);
        free(r);
        return NULL;
    }

    /* Shutdown eventfd wakes the loop. */
    r->wakeup_fd = eventfd(0, EFD_NONBLOCK);
    if (r->wakeup_fd < 0) {
        close(r->epfd);
        free(r->doc_root);
        free(r->conns);
        free(r);
        return NULL;
    }
    struct epoll_event wev;
    wev.events = EPOLLIN;
    wev.data.fd = r->wakeup_fd;
    epoll_ctl(r->epfd, EPOLL_CTL_ADD, r->wakeup_fd, &wev);

    r->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (r->listen_fd < 0) {
        close(r->epfd);
        close(r->wakeup_fd);
        free(r->doc_root);
        free(r->conns);
        free(r);
        return NULL;
    }
    int one = 1;
    setsockopt(r->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (cfg->reuse_port) {
        setsockopt(r->listen_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)cfg->port);
    if (bind(r->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(r->listen_fd, backlog) < 0) {
        close(r->listen_fd);
        close(r->epfd);
        close(r->wakeup_fd);
        free(r->doc_root);
        free(r->conns);
        free(r);
        return NULL;
    }

    /* Resolve the actual bound port (needed when cfg->port was 0). */
    socklen_t slen = sizeof(sa);
    if (getsockname(r->listen_fd, (struct sockaddr *)&sa, &slen) == 0) {
        r->port = ntohs(sa.sin_port);
    } else {
        r->port = cfg->port;
    }

    struct epoll_event lev;
    lev.events = EPOLLIN;
    lev.data.fd = r->listen_fd;
    epoll_ctl(r->epfd, EPOLL_CTL_ADD, r->listen_fd, &lev);

    return r;
}

int reactor_port(const struct reactor *r)
{
    return r->port;
}

int reactor_active_connections(const struct reactor *r)
{
    return __atomic_load_n(&r->active_conns, __ATOMIC_RELAXED);
}

void reactor_run(struct reactor *r)
{
    struct epoll_event *events = malloc(sizeof(struct epoll_event) * (size_t)r->max_events);
    if (!events) {
        return;
    }

    while (!r->stop) {
        int n = epoll_wait(r->epfd, events, r->max_events, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == r->wakeup_fd) {
                r->stop = true;
                break;
            }
            if (fd == r->listen_fd) {
                reactor_accept(r);
                continue;
            }
            struct conn *c = r->conns[fd];
            if (!c) {
                continue;
            }
            uint32_t evs = events[i].events;
            if (evs & (EPOLLERR | EPOLLHUP)) {
                conn_close(r, c);
                continue;
            }
            if (evs & EPOLLIN) {
                conn_read(r, c);
                if (r->conns[fd] == NULL) {
                    continue; /* conn was closed during read */
                }
                /*
                 * Reading may have completed a request and built a response
                 * (state == ST_WRITING). Flush it now: the socket is usually
                 * writable right after a read, so this avoids waiting forever
                 * for an EPOLLOUT event that edge-triggered epoll would not
                 * issue for a never-full send buffer.
                 */
                if (c->state == ST_WRITING) {
                    conn_write(r, c);
                    if (r->conns[fd] == NULL) {
                        continue;
                    }
                }
            }
            if (evs & EPOLLOUT) {
                conn_write(r, c);
            }
        }
    }

    free(events);
}

void reactor_stop(struct reactor *r)
{
    uint64_t one = 1;
    ssize_t ignored = write(r->wakeup_fd, &one, sizeof(one));
    (void)ignored;
}

void reactor_destroy(struct reactor *r)
{
    if (!r) {
        return;
    }
    /* Close any remaining connections. */
    for (int fd = 0; fd < CONN_MAX; fd++) {
        struct conn *c = r->conns[fd];
        if (c) {
            epoll_ctl(r->epfd, EPOLL_CTL_DEL, fd, NULL);
            close(fd);
            if (c->head) {
                free(c->head);
            }
            if (c->file_fd >= 0) {
                close(c->file_fd);
            }
            free(c);
        }
    }
    close(r->listen_fd);
    close(r->wakeup_fd);
    close(r->epfd);
    free(r->conns);
    free(r->doc_root);
    free(r);
}
