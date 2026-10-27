# Contributing

`ringsvr` is a small research/archive project: one C file that serves HTTP/1.1 over a
single `io_uring`. Contributions that keep it small and readable are welcome.

## Scope

In scope:

- Fixes for correctness and safety bugs in the current GET/POST paths.
- Small experiments that make the `io_uring` request state machine clearer or more
  faithful (see the roadmap in [`README.md`](README.md#roadmap)).
- Documentation that matches the code.

Out of scope unless discussed first:

- Turning it into a production server (TLS, auth, config files, worker pools).
- Splitting `main.c` into modules or adding a build system beyond the current CMake file.
- Dependencies other than `liburing`.

## Before you send a PR

1. Build it:

   ```sh
   cmake -S . -B build
   cmake --build build
   ```

2. Exercise it end to end. There is no test suite, so a manual round-trip is the check:
   start `./build/ringsvr` from a scratch directory and confirm GET, POST, `404`, and the
   `..` refusal still behave as described in the README. Debug output is enabled by
   `#define DBG` in `main.c`.
3. Keep the diff focused. One behavior change per PR, no drive-by reformatting.

Include the following in the PR description:

- kernel version (`uname -r`), compiler version, and `liburing` version;
- the request you sent and the response you observed for anything that changes behavior.

## Issues

State what you ran, what you expected, and what happened. For protocol or `io_uring`
problems, include the kernel and `liburing` versions — behavior there is version
dependent. Security reports are just issues here; this project is not maintained as a
production service.
