<p align="center">
  <img src="assets/logo.svg" alt="ringsvr logo" width="160" height="160">
</p>

<p align="center">
  English · <a href="README.zh-CN.md">简体中文</a>
</p>

# ringsvr

*A single-threaded HTTP/1.1 file server for Linux.*

**A single-threaded HTTP/1.1 file server built entirely on `io_uring` — accept, read, open, send, and zero-copy file transfer all run through one ring, with no thread pool.**

`ringsvr` is a compact reference implementation of a server whose entire I/O path is
asynchronous. It is deliberately small enough to read in one sitting, yet it exercises the
parts of `io_uring` that make real servers interesting: SQPOLL submission, tagged
continuation jobs, per-connection pipes, and file-to-socket `splice()`.

It is a research/archive project, not a hardened production server. The whole point is
to show the shape of an `io_uring`-driven request state machine — and where the naive
version starts to hurt.

## Quick start

**Requirements:** Linux with `io_uring` support, a C11 compiler, CMake ≥ 3.16, and
`liburing`.

```sh
# Debian / Ubuntu
sudo apt install build-essential cmake liburing-dev

cmake -S . -B build
cmake --build build

mkdir -p demo
printf 'hello from ringsvr\n' > demo/index.html
./build/ringsvr &

curl -s http://localhost:8000/demo/index.html
# hello from ringsvr

printf 'uploaded\n' > demo/in.txt
curl -s -X POST --data-binary @demo/in.txt http://localhost:8000/demo/out.txt
cat demo/out.txt
# uploaded

kill %1
```

The server binds `0.0.0.0:8000`, has no flags or configuration, and resolves request
paths relative to its working directory. The leading `/` of the URL path is dropped, so
`/demo/index.html` maps to `./demo/index.html`.

## What it can do now

- **GET with zero-copy file transfer**: `openat(O_RDONLY)` → `statx(AT_EMPTY_PATH)` for
  the size → a hand-built response header sent with `MSG_ZEROCOPY` → `splice()` from the
  file into a per-connection pipe and out to the socket.
- **POST straight to a file**: `openat(O_RDWR | O_TRUNC | O_CREAT)` → write the body
  bytes already in the request buffer → `splice()` the remaining socket body through the
  pipe into the file → `200 OK`.
- **One ring for every operation**: accept, recv, openat, statx, send, splice, write, and
  close are all queued as SQEs. No blocking syscalls on the request path.
- **SQPOLL event loop** with `sq_thread_idle = 10` ms, a 1024-entry ring, and 256
  pre-allocated connection slots.
- **Static limits with a clear failure mode**: a missing file returns `404`; a path
  containing `..` is rejected, and a header larger than the 512-byte buffer or a method
  other than `GET`/`POST` closes the connection.
- **Persistent connections**: after a response, the same socket is re-armed for the next
  request.

### Boundaries

There is no TLS, authentication, directory listing, `Content-Type`, `Date`/`Server`
header, `HEAD`, `Range`, chunked encoding, request timeout, or graceful shutdown. Debug
logging is on by default (`#define DBG` in `main.c`), and every completion prints a line.
The 512-byte request buffer does not grow.

## Architecture

The server is one event loop around one `io_uring`:

```text
GET
  accept -> recv -> openat -> statx -> send headers
         -> splice file -> pipe -> socket -> close file -> recv

POST
  accept -> recv -> openat -> write buffered body
         -> splice socket -> pipe -> file -> send 200 -> close file -> recv
```

Each connection slot owns a socket, a file descriptor, a pipe pair, a request buffer, and
the counters for the current transfer:

```c
struct {
    int file;
    int pipe[2];
    int sock;
    size_t srcn, pipen, dstn;
    char buf[512];
    struct statx stat;
    unsigned short bufi, bufn;
} TH[256];
```

Every SQE carries `user_data = (slot_index << 4) | job`. When a completion arrives, the
low four bits select the next state, and the loop re-arms the next operation from the
shared labels. There are no callbacks, coroutines, or worker threads: the completion tag
is the program counter.

| File | Purpose |
| --- | --- |
| `main.c` | The entire server: ring setup, connection table, and request state machine. |
| `CMakeLists.txt` | A minimal C11 build that links `liburing`. |
| `assets/logo.svg` | Project logo used by the README. |
| `docs/project-status.md` | Notes on scope, evidence, and known limitations. |

## How to develop

Build with the standard CMake flow:

```sh
cmake -S . -B build
cmake --build build
```

If `liburing` is installed under a non-system prefix, point CMake at it:

```sh
cmake -S . -B build -DLIBURING_ROOT=/opt/liburing
cmake --build build
```

To silence the per-completion trace, remove or comment out `#define DBG` near the top of
`main.c`. To test a change, run the server from a scratch directory and exercise it with
`curl` as shown in [Quick start](#quick-start).

There is no test suite and no CI. The last recorded verification was a manual build and
end-to-end run on Ubuntu with GCC 13.3 and liburing 2.5: GET returned the file body, a
missing path returned 404, POST wrote the uploaded bytes, and a `..` traversal attempt was
refused. Treat that as a point-in-time result, not a compatibility promise.

## How to contribute

Issues and pull requests are welcome. Keep changes focused, preserve the single-file
spirit of the server, and include the kernel, compiler, and liburing version you tested
with. For behavior changes, show the request and the observed response.

This is archived research code, so large rewrites are less likely to land than small,
well-explained fixes or experiments.

## Roadmap

### Implemented

- [x] `io_uring`-only accept/recv/open/statx/send/splice/write/close path
- [x] SQPOLL submission and a 1024-entry ring
- [x] 256 connection slots with per-connection pipes
- [x] HTTP/1.1 GET with zero-copy file-to-socket transfer
- [x] HTTP/1.1 POST with socket-to-file transfer
- [x] `404` on missing files and a `..` traversal guard
- [x] Persistent connections for follow-up requests
- [x] `MSG_ZEROCOPY` on response headers

### Planned / possible directions

- [ ] A small test harness for GET, POST, 404, traversal, and pipelined requests
- [ ] `Content-Type`, `Date`, and `Server` response headers
- [ ] `HEAD`, `Range`, and chunked transfer encoding
- [ ] Request timeouts, graceful shutdown, and signal handling
- [ ] Correct `MSG_ZEROCOPY` error-queue handling
- [ ] Optional request-buffer growth beyond 512 bytes

### Future vision

If this project is revived, the interesting direction is not “more features.” It is a
faithful, readable reference for what a fully asynchronous file server looks like when the
ring is the scheduler: one connection table, one completion tag, and a transfer path with
no hidden copies. The roadmap above is a list of experiments toward that shape, not a
release commitment.

## License

GNU Affero General Public License v3.0. See [`LICENSE`](LICENSE).
