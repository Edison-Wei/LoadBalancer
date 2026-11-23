# C++20 HTTP Load Balancer

An asynchronous HTTP/1.1 reverse proxy built incrementally with Boost.Asio and
Boost.Beast. The planned design uses per-request round robin, immutable backend
snapshots, active readiness probes, and a small I/O worker pool.

Phase 0 supplies the build foundation: a coroutine timer, a Beast HTTP parser
smoke example, GoogleTest through CTest, and Linux CI. The `load_balancer` binary
currently runs the smoke example and exits. HTTP listening and forwarding will
arrive in later phases.

See [the roadmap](Load_Balancer_Roadmap.md) and
[Phase 0 validation evidence](docs/reports/phase-0.md).

## Prerequisites

- CMake 3.28+, Ninja, Git, and network access to GitHub on first configure.
- C++20 compiler and standard library with coroutines and
  `std::atomic<std::shared_ptr<T>>`. Linux CI targets GCC 14 on Ubuntu 24.04;
  Apple Clang 21 is the available local compiler.
- Boost 1.92.0 (one CMake-ready archive) and GoogleTest v1.18.0 are fetched by
  CMake. No separately installed Boost/GoogleTest is required.

The current local Apple standard library fails the atomic shared-pointer check.
Use the Linux toolchain for validation; local builds also require installing
Ninja and restoring GitHub access. See the evidence report for exact results.

## Build and test

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
./build/debug/load_balancer

cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
```

The `asan-ubsan` and `tsan` presets use independent build directories. For example:

```sh
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan --parallel
ctest --preset asan-ubsan

cmake --preset tsan
cmake --build --preset tsan --parallel
ctest --preset tsan
```

Run TSan separately from ASan/UBSan on a host supported by its runtime. CI retains
configure, build, and test logs plus JUnit results for each preset, even on failure.
For local logs, redirect command output to files or use `tee` with shell pipefail.

Ninja is the preset generator. If unavailable, install it or configure a separate
Makefiles build (CMake must not reuse a directory configured by another generator):

```sh
cmake -S . -B build/debug-make -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug-make --parallel
ctest --test-dir build/debug-make --output-on-failure --no-tests=error
```

## Layout and conventions

- `include/lb/` and `src/`: public headers and implementations.
- `tests/unit/`: GoogleTest cases registered with CTest, with bounded timeouts.
- `cmake/`: sanitizer configuration; `.clang-format`: LLVM style, four-space indent.
- `.github/workflows/ci.yml`: clean Linux Debug, Release, ASan/UBSan, and TSan jobs.

Only implemented sources are listed in CMake. Backend simulation, proxying,
health checks, metrics, Docker deployment, and benchmarks remain on the roadmap.
This is an educational prototype; no TLS, HTTP/2, gRPC, upgrades, or automatic
request retries are implemented.

Licensed under [MIT](LICENSE).
