# ringsvr

A single-file, single-threaded HTTP/1.1 file server whose entire I/O path —
accept, recv, open, statx, send, splice, close — is driven by `io_uring`,
including zero-copy file→socket transfers through per-connection pipes.

It exists as an experiment: an HTTP file server is small enough to keep the whole
request state machine in your head, but it touches every interesting
`io_uring` primitive. So it is the smallest honest vehicle for answering
"what does a fully `io_uring`-driven server actually look like, and where does
the naive design fall over?" — SQPOLL submission, `user_data`-tagged
continuation jobs, `splice()` plumbing, and the cost of doing all of it with no
thread pool.

## Build

The project is plain CMake plus `liburing`. The intended flow is:

```sh
cmake -B build && cmake --build build
```

That reproduces the original workflow, but read this before blaming the code:

* `CMakeLists.txt` declares `cmake_minimum_required(VERSION 4.1)`, which is
  stricter than anything this project needs. On a machine with CMake 3.x,
  configuration stops immediately (see "Verification" below). Lowering it to
  `3.16` is a one-line change and is very likely correct.
* `liburing` **headers and library must be installed** (`liburing-dev` on
  Debian/Ubuntu). They are not vendored, and `CMakeLists.txt` neither finds nor
  checks for them — it just does `target_link_libraries(ringsvr PRIVATE uring)`.

## Run

```sh
cmake -B build && cmake --build build
./build/ringsvr
```

There are no flags and no configuration. It always binds `all interfaces (INADDR_ANY), port 8000`
(`SO_REUSEADDR` + `SO_REUSEPORT`), creates an `io_uring` with
`IORING_SETUP_SQPOLL` and `sq_thread_idle = 10` ms, and starts with 256
connection slots (`TH[256]`), each with its own pipe pair and 512-byte request
buffer.

**All paths are resolved relative to the process working directory, and the
leading `/` of the URL path is dropped.** The server also rejects any path
containing `..`. So from an empty directory:

```sh
mkdir -p /tmp/ringsvr-demo && cd /tmp/ringsvr-demo
echo 'hello from ringsvr' > index.html
/home/z/vibe/ringsvr/build/ringsvr &
curl -v http://localhost:8000/index.html    # 200 + body
curl -i http://localhost:8000/missing       # 404
printf 'binary\x00payload' > in.bin
curl -v -X POST --data-binary @in.bin http://localhost:8000/copy.bin
cmp in.bin copy.bin                          # POST body written verbatim
kill %1
```

Any request other than `GET`/`POST`, a path containing `..`, a missing file,
or a POST without a `Content-Length` is answered with 404 or simply dropped
after the socket is closed. There is no authentication, no TLS, and no
directory listing.

## Implemented vs. not implemented

Implemented:

* `io_uring`-only event loop; no blocking syscalls on the hot path (accept,
  recv, openat, statx, send, splice, close, write are all prep'd SQEs).
* SQPOLL mode (`p.sq_thread_idle = 10`), 256 accept slots, 1024-entry ring.
* HTTP/1.1 **GET**: `openat(O_RDONLY)` → `statx(AT_EMPTY_PATH)` for the size →
  `send()` the hand-built header → `splice()` file→pipe→socket.
* HTTP/1.1 **POST**: `openat(O_RDWR|O_TRUNC|O_CREAT)` → write the body bytes
  already in the 512-byte buffer → `splice()` socket→pipe→file → `200 OK`.
* `404 NOT FOUND` on open failure; `..` traversal guard; per-connection
  pipelining of follow-up requests after a response.
* `MSG_ZEROCOPY` on the response headers.

Not implemented (deliberately or otherwise):

* **No test suite, no CI, no build config beyond the six-line CMake file.**
* No `Content-Type`, no `Date`/`Server` headers, no `HEAD`, no `Range`
  requests, no chunked transfer encoding, no POST form parsing (body is
  written raw to the named file).
* No `recv` buffer growth beyond 512 bytes: a request header larger than the
  buffer is logged and the connection closed.
* No idle/read timeouts, no keep-alive negotiation, no graceful shutdown
  (the loop is `while (1)`), no signal handling, no rate limiting.
* No `MSG_ZEROCOPY` error-queue handling, so a `-ENOBUFS` completion is just
  treated as a send failure and the connection is closed.
* Logging is debug logging: every completion prints a line to stdout, and the
  `DBG` macro at the top of `main.c` is currently **defined**, so the server is
  chatty.
* No licensing, no docs beyond this file.

## Verification

Observed on 2026 (this checkout, host `cmake 3.28.3`, `gcc 13.3.0`):

```
$ ls /usr/include/liburing.h
ls: cannot access '/usr/include/liburing.h': No such file or directory
$ pkg-config --modversion liburing
Package liburing was not found in the pkg-config search path.

$ cmake -B /tmp/ringsvr-build -S /home/z/vibe/ringsvr
CMake Error at CMakeLists.txt:1 (cmake_minimum_required):
  CMake 4.1 or higher is required.  You are running version 3.28.3

-- Configuring incomplete, errors occurred!
```

**As first copied, configuration failed** on the too-high
`cmake_minimum_required(VERSION 4.1)` (this machine has CMake 3.28) and on the
absent system `liburing`. Both were then resolved — the minimum was lowered to
3.16, an optional `-DLIBURING_ROOT` hint was added, and liburing 2.5 was
extracted from the distro package into a local prefix — after which the server
**was compiled, run and exercised end-to-end** (see "Verified build & run"
below). `main.c` itself is byte-identical to the archive.

## Status

`Status: archive/research` — a 2025-12 experiment snapshot kept for reference.
It is not a maintained tool, has no release, no tests, and no compatibility
promises. If you need a real static file server, use one.

## License

**None.** There is no `LICENSE` file and no license header in any source file.
Absent an explicit license, default copyright applies and no rights are
granted. Do not redistribute or reuse until the author adds a license.

## Verified build & run (2026-09-30)

Built and exercised end-to-end on Ubuntu (gcc 13.3, kernel 7.0.0, liburing 2.5
extracted from the distro package into a local prefix because it is not installed
system-wide):

```sh
apt-get download liburing-dev && dpkg-deb -x liburing-dev_*.deb /tmp/lu/ext
cmake -B build -S . -DLIBURING_ROOT=/tmp/lu/ext/usr && cmake --build build
cd /tmp/rstest && echo 'hello' > index.html
/tmp/rstest/../build/ringsvr &          # binds all interfaces on :8000
curl -s http://localhost:8000/index.html   # -> hello
curl -s -o /dev/null -w '%{http_code}' http://localhost:8000/missing  # -> 404
curl -X POST --data-binary @in.bin http://localhost:8000/copy.bin     # -> 200, file written
curl -o /dev/null -w '%{http_code}' 'http://localhost:8000/../etc/passwd'  # -> 404 (guard)
```

Observed results: GET returned the file body, a missing path returned 404, POST
returned 200 and the uploaded bytes landed in `copy.bin`, and a `..` traversal
attempt was refused with 404.

**Two changes were made to make that possible**, both documented rather than
hidden:
- `CMakeLists.txt`: `cmake_minimum_required(VERSION 4.1)` -> `3.16` (4.1 rejected
  this machine's CMake 3.28 before any compiler ran; nothing in the file needs a
  newer CMake), plus an optional `-DLIBURING_ROOT=<prefix>` include/link hint.
  `main.c` is byte-identical to the archive.
- Everything else is verbatim.
