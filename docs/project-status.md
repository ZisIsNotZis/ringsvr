# ringsvr project status

## Classification

Research / archive experiment: a single-file, single-threaded HTTP/1.1 file
server built entirely on `io_uring` (accept, recv, openat, statx, send, splice,
close), used as a vehicle to exercise the whole `io_uring` surface in a small,
readable program.

## Status

Frozen snapshot (last source change 2025-12). Feature-complete for what it set
out to demonstrate — GET and POST round-trips over an `io_uring`-only event
loop — but never hardened, tested, or released. Not maintained.

## Evidence

- `main.c` (~284 effective lines) — the entire server.
  - `io_uring_queue_init_params(1024, &ring, &p)` with `IORING_SETUP_SQPOLL`
    and `sq_thread_idle = 10`.
  - `struct { int file; int pipe[2]; int sock; ... char buf[512]; } TH[256]` —
    256 pre-allocated connection slots, each with its own pipe pair.
  - `getsq()` / `user_data = (size_t)i << 4 | job` — the continuation
    mechanism: every completion encodes slot index + job kind, and the
    `switch (job)` in `main()` re-arms the next SQE by `goto`-ing into shared
    labels (`RECV`, `SPLICE2`, `WRITE`, `SPLICEW2`).
  - GET path: `openat` → `statx(AT_EMPTY_PATH)` → hand-built header via
    `send` → `splice` file→pipe→socket.
  - POST path: `openat(O_RDWR|O_TRUNC|O_CREAT)` → `write` buffered body →
    `splice` socket→pipe→file → `MSG200`.
  - Safety: `strstr(path, "..")` traversal guard; `404` on open failure.
- `CMakeLists.txt` — 6 lines; `add_executable(ringsvr main.c)` +
  `target_link_libraries(ringsvr PRIVATE uring)`.
- Repository surface: `README.md`, `README.zh-CN.md`, `assets/logo.svg`, and this
  status note. No tests, no CI, no lockfile.

## Boundaries

- **Built and exercised in this environment on 2026-09-30 (see README).** The
  first configure failed on `cmake_minimum_required(VERSION 4.1)` against CMake
  3.28.3 and on the absent system `/usr/include/liburing.h`. After lowering the
  minimum to 3.16 and extracting liburing 2.5 to a local prefix, the server was
  compiled and run end-to-end. `main.c` is unchanged.
- No `Content-Type`/`HEAD`/`Range`/chunked encoding; no timeouts, keep-alive
  negotiation, graceful shutdown, or `MSG_ZEROCOPY` error-queue handling.
- 512-byte fixed request buffer; oversized headers close the connection.
- Serves paths relative to the process CWD (leading `/` stripped); no document
  root, no auth, no TLS.
- `Status: archive/research`. Licensed under AGPL-3.0 (see `LICENSE`); there are
  no per-file license headers.

## Verified build (2026-09-30)

Configured, compiled and exercised on Ubuntu (gcc 13.3, kernel 7.0.0) against
liburing 2.5 extracted to a local prefix: GET returns the file, a missing path
gives 404, POST writes the uploaded bytes (200) and `..` traversal is refused.
`CMakeLists.txt` was changed (minimum 4.1 -> 3.16, optional LIBURING_ROOT hint);
`main.c` is verbatim.
