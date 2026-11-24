# Release benchmarks with wrk

`run.py` owns its loopback backend/proxy children and a dedicated native
Prometheus instance. It does not use or change the user's Docker stack. No
benchmark is a CTest test or a CI throughput gate. The build cache must identify
an unsanitized Release build. Native `wrk` and Prometheus executable paths are
required; `snapshot.py` additionally uses a native Grafana distribution.

```sh
cmake --preset release -DFETCHCONTENT_UPDATES_DISCONNECTED=ON &&
cmake --build --preset release --parallel &&
ctest --preset release

python3 benchmarks/run.py \
  --wrk "$(command -v wrk)" \
  --prometheus "$(command -v prometheus)" \
  --duration 30 --warmup 5 --trials 3 --cooldown 5 \
  --output build/benchmarks-long
```

The output directory must be new. For a short exploratory run, use `--duration 3
--warmup 1`; retain at least three trials for comparisons. Every run records the
actual configuration, executable hashes, git revision/dirty state, seed, run
order, platform, CPU count, and descriptor limits. Local tool downloads/builds
used for local measurements live under ignored `build/tools/`; they are not project sources.

## Matrix

Each of three rounds shuffles the 13 cases with seed 8, for 39 measured trials:

| Cases | Backends | Workers | wrk connections |
|---|---:|---:|---:|
| Direct origin | 1 | Origin's single event loop | 100 |
| Proxy scale-out | 1, 3, 5 | 4 | 100 |
| Worker comparison | 3 | 1, 2, 8, auto (`--workers 0`) | 100 |
| Connection comparison | 3 | 4 | 10, 500, 1,000 |
| Quiescent recurring probes | 3 | 4 | 100 |
| Controlled crash/restart | 3 | 4 | 100 |

The shared three-backend/four-worker/100-connection case completes the worker
and connection comparisons. The generator uses two threads by default; all
listed connection counts divide evenly between them. `-c` means open
connections, not users. Workload is GET `/request`, no think time or pipelining,
with downstream keep-alive. The proxy opens a fresh upstream connection per
business request. A consistent Lua response hook counts exact HTTP statuses and
successful response backend IDs. It also emits wrk p50/p95/p99, total completed
HTTP RPS, socket error counts, and structured data. The same hook runs for the
direct baseline, so response parsing overhead is included everywhere.

All proxy trials bootstrap readiness. Normal recurring probes run every 200 ms;
the quiescent case uses a 60-second interval after bootstrap, so it suppresses
recurring probes during short measurements without adding a production option
that bypasses health checks. In the failure case, the harness kills only its B
child about one third into measurement, restarts B on its original port about
two thirds in, and verifies final health recovery. Actual action timestamps are
saved. Expected HTTP errors during detection are counted, not hidden.

Use repeatable `--case NAME` flags to restrict a run, e.g. `--case direct --case
proxy-3b-4w`. Case names and the complete randomized schedule are in metadata.
Seeds repeat run order, not numerical results or kernel scheduling.

## Measurements and evidence

Each trial preserves raw warm-up and measured wrk stdout/stderr, exact status
and backend counts, cumulative raw metrics, a scoped Grafana dashboard definition,
and real Prometheus range-query snapshots for all 15 dashboard targets. Prometheus
scrapes every second in all comparisons. Direct trials have no LB metrics target;
its Prometheus process still runs, but those LB series are inapplicable.

`os.wait4` supplies each owned process's user/system CPU time, lifetime wall time,
peak RSS, context switches, and exit code. macOS RSS is converted from bytes and
Linux from KiB. CPU percentage is relative to one core and covers the process
lifetime, including startup/warm-up/collection/cleanup, **not just the measured
wrk interval**. Peak RSS is not a time series. Generator CPU is recorded as well
as origin/proxy CPU. Sandboxed host `ps`/`sysctl` access can be denied; the report
states unavailable host-wide CPU/network/kernel data explicitly. wrk transfer
counts represent HTTP traffic, not independently sampled NIC utilization.

`results.json` contains every measured trial; `summary.json` reports the median,
minimum, maximum, and sample standard deviation of RPS, plus median latency
quantiles and successful business RPS. A completed HTTP response is not necessarily
a successful business response: HTTP 200 rate and failure fraction are separate.
wrk's standard latency histogram includes error responses too. Do not interpret
fast 503s as low-latency successful work.

At high fresh-connection rates, the environment may return EADDRNOTAVAIL, including
to the generator or monitoring client. An empty generator startup with that exact
error may be retried, at most twelve times with five-second waits; retries are saved.
A completed workload is never retried to select a better RPS. Post-trial monitoring
connections may wait up to 45 seconds, outside the wrk measurement. Monitoring
query ranges can include generator startup retries; wrk's own elapsed time is the
source for reported RPS. HTTP/socket workload errors remain unchanged in results.
Cooldown and randomized ordering reduce carryover but do not eliminate it.

All children are reaped on normal completion or Python exceptions. Failed runs
retain partial results, raw outputs, and resource records; no global kernel
settings or unrelated processes are modified. Per-request app logs remain enabled,
so their synchronization and disk overhead are included. Large stderr logs are
gzipped after measurement.

## Real Grafana snapshot

After a completed benchmark run has stopped its Prometheus process:

```sh
python3 benchmarks/snapshot.py \
  --run build/benchmarks-long \
  --prometheus "$(command -v prometheus)" \
  --grafana-home /path/to/grafana-distribution
```

This starts isolated loopback Prometheus/Grafana processes against the archived
benchmark TSDB. Fresh local credentials permit snapshot
creation in this disposable Grafana database; anonymous access is Viewer-only, and it does not alter an existing
Grafana instance. It provisions the real datasource and dashboard, queries all
15 targets for one representative three-backend trial, embeds the returned data
frames, and archives Grafana's actual snapshot response, query frames, and export
under `grafana-evidence/`. Both processes then stop. Snapshot URLs therefore
expire with that local process; JSON/data artifacts remain. The script does not
claim a browser screenshot.

## Interpretation

The [local results](../docs/benchmark-results.md) use short exploratory trials,
not a production capacity claim. Report medians and spread; do not select only the fastest run. These are
closed-loop, repeating-request measurements on a shared host, so client saturation,
thermal changes, TCP lifecycle state, log I/O, and coordinated omission can affect
results. There is no invented RPS target. Longer trials, independent load-generator
hardware, host-level profiling, and a secondary constant-rate experiment are
appropriate before drawing stronger capacity or tail-latency conclusions.

References: [wrk](https://github.com/wg/wrk),
[wrk Lua scripting](https://github.com/wg/wrk/blob/4.2.0/SCRIPTING).

Generate tables and an optional standalone figure after the run:

```sh
python3 benchmarks/report.py build/benchmarks-long
# Requires matplotlib:
python3 benchmarks/plot.py build/benchmarks-long
```

A trial's `passed` field means measurement capture, response accounting, known
status/identity classification, and cleanup succeeded. It does **not** mean all
requests were HTTP 200. A healthy-config trial can report no successful business
responses under resource pressure; the report retains that outcome instead of
silently dropping the trial. Consult successful business RPS and status counts.
