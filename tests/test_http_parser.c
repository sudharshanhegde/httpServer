/*
 * test_http_parser.c - Checkpoint 1 end-to-end tests for the incremental
 * HTTP/1.1 parser (src/http_parser.c).
 *
 * The parser is pure logic with no sockets/threads, so we exercise it with
 * real byte buffers: full requests, requests split across arbitrary feed()
 * boundaries, one-byte-at-a-time feeding, and malformed inputs. A helper
 * function is a test of one named behavior; a single failure is localized by
 * the test's name.
 */

#include "server.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

/* Parse a whole request string in one feed() call. */
static enum http_parse_status parse_full(const char *text, struct http_request *req)
{
    memset(req, 0, sizeof(*req));
    struct http_parser p;
    http_parser_init(&p, req);
    return http_parser_feed(&p, text, strlen(text));
}

/* Feed the request one byte at a time to prove partial-read handling. */
static enum http_parse_status parse_one_byte(const char *text, struct http_request *req)
{
    memset(req, 0, sizeof(*req));
    struct http_parser p;
    http_parser_init(&p, req);
    enum http_parse_status st = HTTP_PARSE_INCOMPLETE;
    for (size_t i = 0; i < strlen(text); i++) {
        st = http_parser_feed(&p, text + i, 1);
        if (st != HTTP_PARSE_INCOMPLETE) {
            return st;
        }
    }
    return st;
}

/* Feed the request in the middle of the headers to force a split there. */
static enum http_parse_status parse_split_header(const char *text, size_t split, struct http_request *req)
{
    memset(req, 0, sizeof(*req));
    struct http_parser p;
    http_parser_init(&p, req);
    enum http_parse_status st = http_parser_feed(&p, text, split);
    if (st != HTTP_PARSE_INCOMPLETE) {
        return st;
    }
    return http_parser_feed(&p, text + split, strlen(text) - split);
}

static void test_simple_get(void)
{
    const char *r = "GET /index.html HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "\r\n";
    struct http_request req;
    enum http_parse_status st = parse_full(r, &req);
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(req.parsed == true);
    CHECK(req.method == HTTP_GET);
    CHECK(strcmp(req.path, "/index.html") == 0);
    CHECK(strcmp(req.version, "HTTP/1.1") == 0);
    CHECK(req.keep_alive == true);          /* HTTP/1.1 defaults to persistent */
    CHECK(req.num_headers == 1);
    CHECK(strcmp(req.headers[0], "Host: example.com") == 0);
}

static void test_head_method(void)
{
    const char *r = "HEAD /style.css HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "\r\n";
    struct http_request req;
    enum http_parse_status st = parse_full(r, &req);
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(req.method == HTTP_HEAD);
    CHECK(strcmp(req.path, "/style.css") == 0);
}

static void test_post_method(void)
{
    const char *r = "POST /submit HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "Content-Length: 0\r\n"
                    "\r\n";
    struct http_request req;
    enum http_parse_status st = parse_full(r, &req);
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(req.method == HTTP_POST);
}

static void test_unsupported_method(void)
{
    const char *r = "DELETE /x HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "\r\n";
    struct http_request req;
    enum http_parse_status st = parse_full(r, &req);
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(req.method == HTTP_UNSUPPORTED);
}

static void test_query_string_split(void)
{
    const char *r = "GET /search?q=cats&page=2 HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "\r\n";
    struct http_request req;
    enum http_parse_status st = parse_full(r, &req);
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(strcmp(req.path, "/search") == 0);
    CHECK(strcmp(req.query_string, "q=cats&page=2") == 0);
}

static void test_header_value_ows_stripped(void)
{
    const char *r = "GET / HTTP/1.1\r\n"
                    "Host:    example.com\r\n"
                    "User-Agent:   curl/8.0\r\n"
                    "\r\n";
    struct http_request req;
    enum http_parse_status st = parse_full(r, &req);
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(strcmp(req.headers[0], "Host: example.com") == 0);
    CHECK(strcmp(req.headers[1], "User-Agent: curl/8.0") == 0);
}

static void test_one_byte_at_a_time(void)
{
    const char *r = "GET /favicon.ico HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n";
    struct http_request req;
    enum http_parse_status st = parse_one_byte(r, &req);
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(req.method == HTTP_GET);
    CHECK(strcmp(req.path, "/favicon.ico") == 0);
    CHECK(req.num_headers == 2);
    CHECK(req.keep_alive == true);
}

static void test_split_across_feed_boundaries(void)
{
    const char *r = "GET /index.html HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "Accept: text/html\r\n"
                    "\r\n";
    struct http_request req;
    /* Split inside the request line, inside a header name, and inside a value. */
    enum http_parse_status st = parse_split_header(r, 11, &req);
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(req.method == HTTP_GET);
    CHECK(strcmp(req.path, "/index.html") == 0);

    struct http_request req2;
    st = parse_split_header(r, 40, &req2);   /* splits inside "Host: exa..." */
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(req2.num_headers == 2);

    struct http_request req3;
    st = parse_split_header(r, 47, &req3);   /* splits inside "Accept: text..." */
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(req3.num_headers == 2);
}

static void test_http10_defaults_to_close(void)
{
    const char *r = "GET / HTTP/1.0\r\n"
                    "\r\n";
    struct http_request req;
    enum http_parse_status st = parse_full(r, &req);
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(strcmp(req.version, "HTTP/1.0") == 0);
    CHECK(req.keep_alive == false);
    /* HTTP/1.0 does not require a Host header. */
    CHECK(req.num_headers == 0);
}

static void test_http10_keep_alive_header(void)
{
    const char *r = "GET / HTTP/1.0\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n";
    struct http_request req;
    enum http_parse_status st = parse_full(r, &req);
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(req.keep_alive == true);
}

static void test_http11_connection_close(void)
{
    const char *r = "GET / HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "Connection: close\r\n"
                    "\r\n";
    struct http_request req;
    enum http_parse_status st = parse_full(r, &req);
    CHECK(st == HTTP_PARSE_DONE);
    CHECK(req.keep_alive == false);
}

static void test_missing_host_on_http11_rejected(void)
{
    const char *r = "GET / HTTP/1.1\r\n"
                    "Accept: text/html\r\n"
                    "\r\n";
    struct http_request req;
    enum http_parse_status st = parse_full(r, &req);
    CHECK(st == HTTP_PARSE_ERROR);
    CHECK(req.parsed == false);
}

static void test_empty_request_line_rejected(void)
{
    struct http_request req;
    CHECK(parse_full("\r\n\r\n", &req) == HTTP_PARSE_ERROR);
}

static void test_bare_newline_rejected(void)
{
    struct http_request req;
    CHECK(parse_full("\n", &req) == HTTP_PARSE_ERROR);
}

static void test_empty_method_rejected(void)
{
    struct http_request req;
    CHECK(parse_full(" GET / HTTP/1.1\r\n\r\n", &req) == HTTP_PARSE_ERROR);
}

static void test_missing_target_rejected(void)
{
    struct http_request req;
    CHECK(parse_full("GET  HTTP/1.1\r\n\r\n", &req) == HTTP_PARSE_ERROR);
}

static void test_empty_header_name_rejected(void)
{
    const char *r = "GET / HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    ": bad\r\n"
                    "\r\n";
    struct http_request req;
    CHECK(parse_full(r, &req) == HTTP_PARSE_ERROR);
}

static void test_bare_cr_in_headers_rejected(void)
{
    const char *r = "GET / HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "X: y\r\n"
                    "Z: w\rX\r\n"
                    "\r\n";
    struct http_request req;
    CHECK(parse_full(r, &req) == HTTP_PARSE_ERROR);
}

static void test_incomplete_is_reported(void)
{
    struct http_request req;
    enum http_parse_status st = parse_full("GET /index.html HTTP/1.1\r\nHost: exa", &req);
    CHECK(st == HTTP_PARSE_INCOMPLETE);
    CHECK(req.parsed == false);
}

static void test_method_str(void)
{
    CHECK(strcmp(http_method_str(HTTP_GET), "GET") == 0);
    CHECK(strcmp(http_method_str(HTTP_HEAD), "HEAD") == 0);
    CHECK(strcmp(http_method_str(HTTP_POST), "POST") == 0);
    CHECK(strcmp(http_method_str(HTTP_UNSUPPORTED), "UNSUPPORTED") == 0);
}

int main(void)
{
    test_simple_get();
    test_head_method();
    test_post_method();
    test_unsupported_method();
    test_query_string_split();
    test_header_value_ows_stripped();
    test_one_byte_at_a_time();
    test_split_across_feed_boundaries();
    test_http10_defaults_to_close();
    test_http10_keep_alive_header();
    test_http11_connection_close();
    test_missing_host_on_http11_rejected();
    test_empty_request_line_rejected();
    test_bare_newline_rejected();
    test_empty_method_rejected();
    test_missing_target_rejected();
    test_empty_header_name_rejected();
    test_bare_cr_in_headers_rejected();
    test_incomplete_is_reported();
    test_method_str();

    if (g_failures == 0) {
        printf("test_http_parser: all tests passed\n");
        return 0;
    }
    fprintf(stderr, "test_http_parser: %d test(s) FAILED\n", g_failures);
    return 1;
}
