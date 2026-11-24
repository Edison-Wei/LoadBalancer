# C++ Load Balancer

An asynchronous HTTP/1.1 reverse proxy built with **C++20, Boost.Asio, and
Boost.Beast**. It distributes requests across healthy backends, detects failures,
and restores recovered backends automatically.

The project  a small proxy implementation with a controllable backend
simulator, integration and chaos tests, monitoring, and reproducible benchmark
tooling.

## Why use it?

- **Per-request round robin** across immutable snapshots of eligible backends.
- **Active readiness checks** with failure/recovery thresholds and fail-closed routing.
- **Bounded concurrency** with a configurable worker pool, connection/request limits,
  header/body limits, and independent I/O deadlines.
- **Graceful shutdown** that drains admitted work and cancels remaining operations.
- **Observability** through structured logs, a Prometheus endpoint, and a
 Grafana dashboard.
- **Verify behavior** through unit tests, real HTTP integration tests, seeded
  fault scenarios, and release-archive checks.


## Get started

```sh
git clone https://github.com/Edison-Wei/LoadBalancer.git
cd LoadBalancer
```

### Run the full stack with Docker

Requires Docker with the `docker compose` command:

```sh
docker compose up --build --wait --wait-timeout 120
curl http://127.0.0.1:8080/request
```

The stack starts three test backends, the proxy, Prometheus, and Grafana.
Published ports bind to localhost. The backends and the metrics port are unpublished.

| Service | Address |
|---|---|
| Application | <http://127.0.0.1:8080/request> |
| Grafana dashboard | <http://127.0.0.1:3000/d/load-balancer> |
| Prometheus targets | <http://127.0.0.1:9090/targets> |

Inspect logs or stop the stack:

```sh
docker compose logs -f load-balancer
docker compose down
```

Stopping the stack preserves its named data volumes. See the
[monitoring guide](monitoring/README.md) for validation, metrics, and failure demonstrations.

### Build and run natively

Requirements:

- CMake 3.28+, Ninja, Git, and a C++20 compiler/standard library with coroutine support.
- Python 3.11+ and `curl` for developer tooling and integration tests.
- Network access **required** on the first configure to fetch pinned Boost 1.92.0 and GoogleTest v1.18.0.

Linux CI is configured for GCC 14 on Ubuntu 24.04. Local validation also covers
Apple Clang 21 on macOS. Snapshot publication uses a mutex fallback when the
standard library lacks atomic shared-pointer support.

```sh
cmake --preset release &&
cmake --build --preset release --parallel &&
ctest --preset release
```

If the Docker stack is running, stop it before using the same application port.
Start each backend in a separate terminal:

```sh
./build/release/test_backend --id A --port 9001
```

```sh
./build/release/test_backend --id B --port 9002
```

```sh
./build/release/test_backend --id C --port 9003
```

Start the proxy in another terminal:

```sh
./build/release/load_balancer \
  --backend A,127.0.0.1,9001 \
  --backend B,127.0.0.1,9002 \
  --backend C,127.0.0.1,9003
```

```sh
curl http://127.0.0.1:8080/request
curl -X POST --data-binary 'hello world' http://127.0.0.1:8080/echo
curl http://127.0.0.1:9091/metrics
```

Use `--help` for configuration options and `--version` for JSON build inside the curl request.
Ctrl+C to exit out with a graceful proxy shutdown.

## Test and benchmark

CTest runs unit tests and owned-process integration tests. To run the
seeded chaos scenarios:

```sh
ctest --preset release -L chaos
```

For a custom seed or saved-run replay, use the [chaos guide](tests/chaos/README.md).
The harness starts its own children and leaves the Docker stack untouched.

Benchmarking requires native `wrk` and Prometheus executables. Build Release
first, then run a short direct-versus-proxy comparison:

```sh
python3 benchmarks/run.py \
  --wrk "$(command -v wrk)" \
  --prometheus "$(command -v prometheus)" \
  --case direct --case proxy-3b-4w \
  --duration 3 --warmup 1 --trials 3 --cooldown 5 \
  --output build/benchmarks-quick &&
python3 benchmarks/report.py build/benchmarks-quick
```

Use a fresh output directory for each run and run load experiments separately.
The Python runner owns its services; a Lua hook inside wrk validates response
identities and counts statuses. Read **successful business RPS** alongside latency
and errors: total HTTP RPS includes failed responses.

See the [benchmark guide](benchmarks/README.md) for the full matrix, longer runs,
and snapshots. The [local exploratory results](docs/benchmark-results.md) document
observed resource errors and do not establish production capacity.


## Maintainers

Maintained by [Edison Wei](https://github.com/Edison-Wei)