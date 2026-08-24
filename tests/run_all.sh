#!/usr/bin/env bash
# run_all.sh - CI-less local test runner.
#
# Compiles and runs every per-checkpoint test binary under the address and
# undefined-behavior sanitizers. Each checkpoint appends its own build/run
# lines to this script; it is never replaced wholesale.
#
# A test exits nonzero on failure, so `set -e` aborts the whole run at the
# first red test (which is exactly what we want: fix it, don't bury it).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_BIN_DIR="$ROOT/tests/bin"
mkdir -p "$TEST_BIN_DIR"

SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
WARN_FLAGS="-Wall -Wextra -Wpedantic -Werror"

echo "==> Building and running tests..."

# ---------------------------------------------------------------
# Checkpoint 1: HTTP/1.1 state-machine parser
# ---------------------------------------------------------------
echo "    [1/4] http_parser"
CC="${CC:-gcc}"
"$CC" $WARN_FLAGS $SAN_FLAGS -I"$ROOT/include" \
    "$ROOT/tests/test_http_parser.c" "$ROOT/src/http_parser.c" \
    -o "$TEST_BIN_DIR/test_http_parser"
"$TEST_BIN_DIR/test_http_parser"

# ---------------------------------------------------------------
# Checkpoint 2: generic open-addressing hash table
# ---------------------------------------------------------------
echo "    [2/4] hash_table"
"$CC" $WARN_FLAGS $SAN_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_hash_table.c" "$ROOT/src/hash_table.c" \
    -o "$TEST_BIN_DIR/test_hash_table"
"$TEST_BIN_DIR/test_hash_table"

# ---------------------------------------------------------------
# Checkpoint 3: thread-safe LRU cache on the hash table
# ---------------------------------------------------------------
echo "    [3/4] lru_cache"
"$CC" $WARN_FLAGS $SAN_FLAGS -D_GNU_SOURCE -pthread -I"$ROOT/src" \
    "$ROOT/tests/test_lru_cache.c" "$ROOT/src/lru_cache.c" "$ROOT/src/hash_table.c" \
    -o "$TEST_BIN_DIR/test_lru_cache"
"$TEST_BIN_DIR/test_lru_cache"

# ---------------------------------------------------------------
# Checkpoint 4: edge-triggered epoll reactor (real socket, real client)
# ---------------------------------------------------------------
echo "    [4/4] reactor"
"$CC" $WARN_FLAGS $SAN_FLAGS -D_GNU_SOURCE -pthread -I"$ROOT/include" -I"$ROOT/src" \
    "$ROOT/tests/test_reactor.c" "$ROOT/src/reactor.c" "$ROOT/src/http_parser.c" \
    -o "$TEST_BIN_DIR/test_reactor"
"$TEST_BIN_DIR/test_reactor"

echo "==> All tests passed."
