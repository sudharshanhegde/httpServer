# C HTTP/1.1 Static File Server

A non-blocking HTTP/1.1 static file server written from scratch in C with **no
third-party libraries**.

## What it does

Serves files from a document root over HTTP/1.1:

- Parses requests with a hand-written, incremental state-machine parser.
- Serves each response body **zero-copy** with `sendfile()`.
- Handles many concurrent connections via an **edge-triggered epoll** event
  loop, spread across a **dynamically tuned worker pool**.
- Supports keep-alive connections, `GET` and `HEAD`, correct `Content-Type`
  mapping, and returns proper `400`/`403`/`404`/`405`/`500` responses.
- Rejects path-traversal (`..`) request targets.

## Components (`src/`)

| File | Responsibility |
|------|----------------|
| `http_parser.c` | Incremental HTTP/1.1 request-line + header parser |
| `hash_table.c` | Generic open-addressing hash table (thread-agnostic) |
| `lru_cache.c`  | Thread-safe LRU cache built on the hash table |
| `reactor.c`    | Edge-triggered epoll event loop + sendfile data plane |
| `thread_pool.c`| Dynamically tuned pool of per-thread reactors |
| `config.c`     | Parses the configuration file |
| `logger.c`     | Timestamped, mutex-guarded logging |
| `main.c`       | Entry point: config + logger + pool + signal-driven shutdown |

Public types live in `include/server.h`; each module has a self-contained
header under `src/`.

## Build

```sh
make            # builds bin/httpd (-Wall -Wextra -Werror)
make clean
```

## Run

```sh
./bin/httpd [config-file]      # default: server.conf
```

Then e.g. `curl http://127.0.0.1:8080/`. The server serves files from
`server_root` (`./www` by default), maps `/` to `/index.html`, and shuts down
cleanly on `SIGINT`/`SIGTERM`.

Configuration (`server.conf`): `port`, `server_root`, `num_threads` (max
workers), `log_file`, `timeout_secs`.

## Test

```sh
bash tests/run_all.sh
```

Every test compiles with `-Wall -Wextra -Wpedantic -Werror` and runs under
AddressSanitizer + UndefinedBehaviorSanitizer. Tests are real end-to-end
exercises (real sockets, real threads, real files) — no mocks:

- `test_http_parser` — parser behavior (partial reads, malformed input, Host).
- `test_hash_table` — hash table (collisions, resize, random-key stress).
- `test_lru_cache` — LRU (eviction, capacity, stats) + concurrent stress.
- `test_reactor` — reactor over a real bound socket (GET/HEAD/keep-alive/404,
  partial reads, traversal, sendfile of an 8 MiB file, concurrent clients).
- `test_thread_pool` — pool scaling up under load and back down.
- `test_integration` — full lifecycle + concurrency + soak + config/logger.
- `test_parser_fuzz` — fuzzes the parser (structured + random) under ASan/UBSan.
