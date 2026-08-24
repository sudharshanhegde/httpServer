/*
 * test_parser_fuzz.c - Hardening: fuzz the HTTP parser.
 *
 * The parser is the only component that consumes untrusted bytes, so it gets
 * a fuzz pass. This harness feeds it structured seeds (valid requests and
 * hand-written malformed variants), byte-mutations of those seeds, and purely
 * random buffers — both all-at-once and in random chunk sizes — while running
 * under ASan/UBSan. A heap/stack/UB bug aborts the harness (nonzero exit), so
 * "all passed" means no memory-safety or undefined-behavior fault was found
 * across hundreds of thousands of inputs. A deterministic PRNG keeps the run
 * reproducible.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_rng = 0x12345678u;

static uint32_t rng(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static uint8_t rand_byte(void)
{
    return (uint8_t)(rng() & 0xff);
}

/* Feed @data to a fresh parser, either at once or in random chunk sizes. */
static void feed_one(const uint8_t *data, size_t n, int chunked)
{
    struct http_request req;
    memset(&req, 0, sizeof(req));
    struct http_parser p;
    http_parser_init(&p, &req);

    size_t off = 0;
    while (off < n) {
        size_t c = chunked ? (1u + rng() % 16u) : (n - off);
        if (c > n - off) {
            c = n - off;
        }
        http_parser_feed(&p, (const char *)data + off, c);
        off += c;
    }
}

enum { SEEDS = 12, ITERS = 200000 };

static const char *const g_seeds[SEEDS] = {
    "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
    "GET /index.html?q=1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
    "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n",
    "POST /submit HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n",
    "GET /a/b/c HTTP/1.0\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\nUser-Agent: fuzz\r\n\r\n",
    "\r\n\r\n",
    "\n",
    "GET  HTTP/1.1\r\n\r\n",
    "GET / HTTP/1.1\r\n: bad\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: x",
    "G",
};

int main(void)
{
    /* Structured fuzz: seeds and mutated/truncated seeds. */
    for (int i = 0; i < ITERS; i++) {
        int mode = (int)(rng() % 3);
        if (mode == 0) {
            /* A whole seed verbatim. */
            const char *s = g_seeds[rng() % SEEDS];
            feed_one((const uint8_t *)s, strlen(s), (int)(rng() & 1));
        } else if (mode == 1) {
            /* A seed with a random truncation point. */
            const char *s = g_seeds[rng() % SEEDS];
            size_t n = strlen(s);
            size_t cut = (n == 0) ? 0 : (rng() % (n + 1));
            feed_one((const uint8_t *)s, cut, (int)(rng() & 1));
        } else {
            /* A seed with a handful of random byte flips. */
            const char *s = g_seeds[rng() % SEEDS];
            char buf[512];
            size_t n = strlen(s);
            memcpy(buf, s, n);
            int flips = 1 + (int)(rng() % 4);
            for (int f = 0; f < flips && n > 0; f++) {
                buf[rng() % n] = (char)rand_byte();
            }
            feed_one((const uint8_t *)buf, n, (int)(rng() & 1));
        }
    }

    /* Pure random buffers of assorted sizes. */
    for (int i = 0; i < 100000; i++) {
        uint8_t buf[64];
        size_t n = 1 + (size_t)(rng() % sizeof(buf));
        for (size_t j = 0; j < n; j++) {
            buf[j] = rand_byte();
        }
        feed_one(buf, n, (int)(rng() & 1));
    }

    printf("test_parser_fuzz: no faults across %d inputs\n", ITERS + 100000);
    return 0;
}
