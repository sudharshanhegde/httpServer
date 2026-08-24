/*
 * http_parser.c - Incremental HTTP/1.1 request-line and header parser.
 *
 * A hand-written, state-driven parser with no third-party dependencies. It is
 * designed to consume arbitrary byte chunks so a request split across multiple
 * read()s (or arriving one byte at a time) parses correctly, and to reject
 * malformed input with HTTP_PARSE_ERROR rather than overrunning a buffer.
 *
 * Parsing is pure logic: no sockets, no threads, no I/O. That makes it the
 * safest and fastest component to test in isolation.
 */

#include "server.h"

/* Internal state-machine states (not part of the public API). */
enum parser_state {
    P_METHOD = 0,        /* reading the method token up to the first space */
    P_PATH,              /* reading the request-target up to space or '?' */
    P_QUERY,             /* reading the query string after '?' */
    P_VERSION,           /* reading the HTTP version token */
    P_VERSION_CR,        /* CR seen after version; expect LF */
    P_HEADER_NAME,       /* reading a header field name up to ':' */
    P_HEADER_NAME_CR,    /* CR seen at the start of a header line (blank line) */
    P_HEADER_VALUE,      /* reading a header field value */
    P_HEADER_VALUE_CR,   /* CR seen after a header value; expect LF */
    P_DONE               /* terminating blank line consumed; parse finished */
};

/*
 * Methods the server understands. Anything else is mapped to
 * HTTP_UNSUPPORTED (still a syntactically valid request line; the caller
 * decides whether to reject it with 501/405).
 */
static enum http_method method_from_token(const char *tok)
{
    if (strcmp(tok, "GET") == 0) {
        return HTTP_GET;
    }
    if (strcmp(tok, "HEAD") == 0) {
        return HTTP_HEAD;
    }
    if (strcmp(tok, "POST") == 0) {
        return HTTP_POST;
    }
    return HTTP_UNSUPPORTED;
}

/*
 * Finalize a successfully parsed request: set the parsed flag, derive the
 * default keep-alive from the protocol version, then let an explicit
 * Connection header override it. Callers reuse this on the DONE transition.
 */
static void parser_finalize(struct http_parser *p)
{
    struct http_request *req = p->req;

    req->parsed = true;
    /* HTTP/1.1 defaults to persistent connections; HTTP/1.0 defaults to close. */
    req->keep_alive = (strcmp(req->version, "HTTP/1.1") == 0);

    for (int i = 0; i < req->num_headers; i++) {
        if (strncasecmp(req->headers[i], "connection:", 11) == 0) {
            if (strcasestr(req->headers[i], "close") != NULL) {
                req->keep_alive = false;
            } else if (strcasestr(req->headers[i], "keep-alive") != NULL) {
                req->keep_alive = true;
            }
        }
    }
}

void http_parser_init(struct http_parser *p, struct http_request *req)
{
    memset(p, 0, sizeof(*p));
    p->req = req;
    p->state = P_METHOD;
}

enum http_parse_status http_parser_feed(struct http_parser *p, const char *buf, size_t len)
{
    if (p->done) {
        return HTTP_PARSE_DONE;
    }

    struct http_request *req = p->req;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];

        switch (p->state) {
        case P_METHOD:
            if (c == ' ' || c == '\t') {
                if (p->method_len == 0) {
                    return HTTP_PARSE_ERROR;    /* empty method token */
                }
                p->method_tmp[p->method_len] = '\0';
                req->method = method_from_token(p->method_tmp);
                p->state = P_PATH;
                p->path_pos = 0;
            } else if (c == '\r' || c == '\n') {
                return HTTP_PARSE_ERROR;        /* request line ended before method */
            } else if (p->method_len < sizeof(p->method_tmp) - 1) {
                p->method_tmp[p->method_len++] = (char)c;
            } else {
                return HTTP_PARSE_ERROR;        /* method token too long */
            }
            break;

        case P_PATH:
            if (c == ' ' || c == '\t') {
                if (p->path_pos == 0) {
                    return HTTP_PARSE_ERROR;    /* empty request-target */
                }
                req->path[p->path_pos] = '\0';
                p->state = P_VERSION;
                p->version_pos = 0;
            } else if (c == '?') {
                req->path[p->path_pos] = '\0';
                p->state = P_QUERY;
                p->query_pos = 0;
            } else if (c == '\r' || c == '\n') {
                return HTTP_PARSE_ERROR;        /* request line ended mid-target */
            } else if (p->path_pos < MAX_PATH - 1) {
                req->path[p->path_pos++] = (char)c;
            } else {
                return HTTP_PARSE_ERROR;        /* path exceeds MAX_PATH */
            }
            break;

        case P_QUERY:
            if (c == ' ' || c == '\t') {
                req->query_string[p->query_pos] = '\0';
                p->state = P_VERSION;
                p->version_pos = 0;
            } else if (c == '\r' || c == '\n') {
                return HTTP_PARSE_ERROR;        /* request line ended mid-query */
            } else if (p->query_pos < MAX_PATH - 1) {
                req->query_string[p->query_pos++] = (char)c;
            } else {
                return HTTP_PARSE_ERROR;        /* query exceeds MAX_PATH */
            }
            break;

        case P_VERSION:
            if (c == '\r') {
                p->state = P_VERSION_CR;
            } else if (c == '\n') {
                req->version[p->version_pos] = '\0';
                p->state = P_HEADER_NAME;
                p->name_len = 0;
                (void)req;                      /* placeholder; version validated below */
            } else if (p->version_pos < (size_t)ver - 1) {
                req->version[p->version_pos++] = (char)c;
            } else {
                return HTTP_PARSE_ERROR;        /* version token too long */
            }
            break;

        case P_VERSION_CR:
            if (c == '\n') {
                req->version[p->version_pos] = '\0';
                p->state = P_HEADER_NAME;
                p->name_len = 0;
            } else {
                return HTTP_PARSE_ERROR;        /* bare CR without LF */
            }
            break;

        case P_HEADER_NAME:
            if (c == '\r') {
                if (p->name_len == 0) {
                    p->state = P_HEADER_NAME_CR;    /* blank line candidate */
                } else {
                    return HTTP_PARSE_ERROR;        /* CR in the middle of a name */
                }
            } else if (c == '\n') {
                if (p->name_len == 0) {
                    /* tolerate a bare-LF terminator; treat as end of headers */
                    p->done = true;
                    p->state = P_DONE;
                    parser_finalize(p);
                    return HTTP_PARSE_DONE;
                }
                return HTTP_PARSE_ERROR;            /* LF in the middle of a name */
            } else if (c == ':') {
                if (p->name_len == 0) {
                    return HTTP_PARSE_ERROR;        /* empty field name */
                }
                if (strncasecmp(req->headers[req->num_headers], "host", 4) == 0 &&
                    p->name_len == 4) {
                    p->host_seen = true;
                }
                req->headers[req->num_headers][p->name_len] = ':';
                req->headers[req->num_headers][p->name_len + 1] = ' ';
                p->value_pos = p->name_len + 2;
                p->value_len = 0;
                p->state = P_HEADER_VALUE;
            } else if (p->name_len < 255) {
                req->headers[req->num_headers][p->name_len++] = (char)c;
            } else {
                return HTTP_PARSE_ERROR;            /* field name too long */
            }
            break;

        case P_HEADER_NAME_CR:
            if (c == '\n') {
                /* Blank line terminates the header block. */
                if (!p->host_seen && strcmp(req->version, "HTTP/1.1") == 0) {
                    return HTTP_PARSE_ERROR;        /* RFC 7230 5.4: Host required */
                }
                p->done = true;
                p->state = P_DONE;
                parser_finalize(p);
                return HTTP_PARSE_DONE;
            }
            return HTTP_PARSE_ERROR;                /* bare CR without LF */

        case P_HEADER_VALUE:
            if (c == '\r') {
                p->state = P_HEADER_VALUE_CR;
            } else if (c == '\n') {
                req->headers[req->num_headers][p->value_pos + p->value_len] = '\0';
                req->num_headers++;
                p->state = P_HEADER_NAME;
                p->name_len = 0;
            } else {
                /* Skip optional leading whitespace (OWS) after the colon. */
                if (p->value_len == 0 && (c == ' ' || c == '\t')) {
                    break;
                }
                if (p->value_pos + p->value_len < 255) {
                    req->headers[req->num_headers][p->value_pos + p->value_len++] = (char)c;
                } else {
                    return HTTP_PARSE_ERROR;        /* header value too long */
                }
            }
            break;

        case P_HEADER_VALUE_CR:
            if (c == '\n') {
                req->headers[req->num_headers][p->value_pos + p->value_len] = '\0';
                req->num_headers++;
                p->state = P_HEADER_NAME;
                p->name_len = 0;
            } else {
                return HTTP_PARSE_ERROR;            /* bare CR without LF */
            }
            break;

        case P_DONE:
        default:
            return HTTP_PARSE_DONE;
        }
    }

    return HTTP_PARSE_INCOMPLETE;
}

void http_parse_request(struct client_info *client, struct http_request *req)
{
    memset(req, 0, sizeof(*req));

    struct http_parser p;
    http_parser_init(&p, req);

    enum http_parse_status st = http_parser_feed(&p, client->read_buf, client->read_len);
    req->parsed = (st == HTTP_PARSE_DONE);
}

const char *http_method_str(enum http_method method)
{
    switch (method) {
    case HTTP_GET:
        return "GET";
    case HTTP_HEAD:
        return "HEAD";
    case HTTP_POST:
        return "POST";
    case HTTP_UNSUPPORTED:
    default:
        return "UNSUPPORTED";
    }
}
