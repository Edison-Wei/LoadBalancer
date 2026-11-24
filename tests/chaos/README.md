# Seeded failure testing

The standard-library Python harness launches its own loopback-only origins
and load balancer. It does not use or alter the existing Docker stack. A/B are
real C++ test backends; C is a separate, owned Python HTTP fixture for protocol
and readiness faults unavailable in the simulator. The proxy and real C++ origins
use the selected build preset, including sanitizer instrumentation.

Run the two registered seeds:

```sh
cmake --preset debug -DFETCHCONTENT_UPDATES_DISCONNECTED=ON &&
cmake --build --preset debug --parallel &&
ctest --preset debug -L chaos
```

Run another seed explicitly:

```sh
python3 tests/integration/chaos_process.py \
  --backend build/debug/test_backend \
  --proxy build/debug/load_balancer \
  --seed 2026 --logs build/debug/Testing/chaos-2026
```

Replay a saved successful or failed report into a **different** output directory:

```sh
python3 tests/integration/chaos_process.py \
  --backend build/debug/test_backend \
  --proxy build/debug/load_balancer \
  --replay build/debug/Testing/chaos-2026/report.json \
  --logs build/debug/Testing/chaos-2026-replay
```

The seed uses Python's explicitly initialized Mersenne Twister RNG to select the
single victim, multi-crash victims, total-outage restart order, repeated-restart
victims, and business-fault order. Each report stores the plan, seed, SHA-256 of
the canonical plan, Python version, and binary paths. Replay validates that plan
against the supported schema before starting any process. Wall-clock timing,
request interleavings, ephemeral ports, throughput, and failure counts naturally
vary between runs. Repeatability means the same planned faults and verified
invariants; it does not mean identical OS scheduling or identical measurements.
The harness uses observed-state barriers rather than assuming a scheduled fault
has already been detected.

## Scenarios and invariants

1. All-healthy control before faults: successful requests with valid correlated
   request IDs and backend identities.
2. Crash one seeded victim during active traffic; wait for observed exclusion,
   verify no new requests route to it, restart on its original port, verify rejoin.
3. Kill two seeded backends before waiting for either child to exit; verify the
   survivor serves, then restore both. This models one multi-backend crash event.
4. Kill all origins; once every backend is excluded, newly started requests must
   return 503 consistently. Restore all and verify healthy traffic.
5. In seeded order, make C delay business responses past the 250 ms upstream
   deadline (504), reset its business sockets (502), or return HTTP 500 while
   readiness remains 200. Verify error classifications and timeout/reset metrics.
6. False-negative readiness: C returns health 503 while direct business requests
   still succeed; verify exclusion and threshold-based rejoin after restoration.
7. Alternate readiness failure/success for at least five failures; verify the
   three-failure threshold prevents unhealthy-state churn.
8. Repeatedly kill/restart seeded victims while requests are active; finish with
   a fresh healthy control window and all three eligibility gauges equal to one.

Four continuous workers send a request every completion plus a 5 ms pacing gap.
Each successful business response must echo its unique supplied request ID and
identify A/B/C. Unexpected transport exceptions, malformed identities/correlation,
HTTP codes outside 200/500/502/503/504, responses taking over four seconds, a proxy
crash, or cleanup failures fail the run. Healthy and post-exclusion windows have
stricter scenario-specific assertions. Readiness transitions must be observed
within five seconds, a generous test bound around the configured 40 ms intervals,
three failures, and two successes; reports retain actual measured delays.

An in-flight request may fail when its selected backend crashes. Detection-window
errors are counted and classified, rather than requiring zero failures. Fresh
requests started after observed exclusion must use only the remaining pool, or
return 503 if the pool is empty. A good readiness response does not imply that
business responses will be good; business faults alone do not cause passive
health ejection in this implementation.

## Evidence and cleanup

Each output directory contains:

| File | Content |
|---|---|
| `report.json` | Seed/plan/hash, outcome, eight checks, exit codes, windows, detection/recovery observations, failures |
| `events.jsonl` | Seeded event stream, monotonic elapsed times and epoch timestamps, injected/restored faults and observed transitions |
| `requests.jsonl` | Every worker request ID, echoed ID, backend identity, start/end, duration, status, transport exception class |
| `metrics.jsonl` | Time-aligned raw endpoint captures before/during/after faults and during health polling |
| `*.stderr.log` | Proxy health/proxy logs and per-generation backend logs |

Injection time, first observed probe failure/success, LB health-transition time,
and harness observation time are separate fields. Detection-window reports count
completions in the injection-to-observed-exclusion interval, including requests
already in flight at injection. Additional fields count requests started inside
that interval, including failures completed after exclusion. They report response
rate, status breakdown, and nearest-rank p95/p99. Stable before/during/after
windows likewise report completed response rate and latency percentiles. Empty
intervals have null quantiles, not invented latency measurements.

These short, paced checks are functional chaos measurements, **not capacity or
benchmark results**. Release/wrk comparisons remain separate.

Every SIGKILL/SIGTERM is sent through a `Popen` handle created by this harness;
there are no process-name or port-based kills. Faulted children are reaped and
restarted on their original owned ports; final cleanup stops/reaps all remaining
children and joins traffic workers. A SIGTERM interruption enters cleanup, and
an initial report containing the seed/plan exists even if execution is interrupted.
Failures retain their report/events/request/metric/log files for replay. CI uploads
both seeds' complete evidence for every build/snapshot matrix entry.
