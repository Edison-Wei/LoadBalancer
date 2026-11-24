# Local monitoring stack

The stack runs three test origins, the load balancer, Prometheus, and Grafana.
Only the application (8080), Prometheus (9090), and Grafana (3000) publish ports,
all bound to host loopback. Origins and port 9091 stay on the private Compose
network. Services publishing ports also join the normal `host-access` bridge:
Docker versions that omit host port publication for internal-only containers need
this attachment. Origins join only `monitoring`; metrics port 9091 is never
published. The proxy's standalone default metrics address is `127.0.0.1:9091`;
Compose explicitly binds `0.0.0.0:9091` inside that private network.

From the repository root:

```sh
python3 tests/integration/monitoring_config.py &&
docker compose up --build --wait --wait-timeout 120 &&
python3 tests/integration/monitoring_stack.py
```

Open <http://127.0.0.1:3000/d/load-balancer> for the provisioned dashboard and
<http://127.0.0.1:9090/targets> for scrape health. Anonymous access is Viewer-only
for this local demonstration. Prometheus scrapes every five seconds; rates and
quantiles require multiple samples and traffic. Generate traffic with:

```sh
for n in $(seq 1 120); do
  curl --fail --silent http://127.0.0.1:8080/request
  sleep 0.1
done
```

`monitoring_stack.py` uses real APIs to verify `up=1`, nonzero request rate,
three healthy origins, the provisioned datasource/dashboard, and all panel
queries. It saves results to `build/monitoring-evidence/`. It sends requests but
does not start or stop services. The separate CI monitoring job starts its own
stack, checks configuration/exposition with `promtool`, runs this API check,
saves logs, then stops its stack. Local CTest does not require Docker.

For a manual controlled failure of this stack's B container:

```sh
docker compose stop backend-b
# Observe B's health fall to zero after three failed probes, while A/C serve.
curl --silent http://127.0.0.1:8080/request
docker compose start backend-b
# Observe recovery after two successful probes.
```

Use the timestamped `health_probe` and `health_transition` events as the
companion event log for the dashboard's eligibility and transition panels:

```sh
docker compose logs --no-color --timestamps load-balancer
```

Readiness probes, transport failures, HTTP 4xx/5xx responses, and cancellations
are distinct. A backend returning HTTP 500 can still be healthy. The dashboard
includes total/per-backend RPS, status classes, 5xx fraction, p50/p95/p99 latency,
backend eligibility, state transitions, active sockets/in-flight requests,
readiness failures, upstream errors/timeouts, and capacity rejections. CPU/RSS
requires an additional exporter and is intentionally absent.

To inspect and lint the actual text without publishing metrics:

```sh
docker compose exec -T prometheus promtool check config /etc/prometheus/prometheus.yml
docker compose exec -T load-balancer curl --fail --silent http://127.0.0.1:9091/metrics |
  docker compose exec -T prometheus promtool check metrics
```

Stop with `docker compose down`. Named volumes retain monitoring data; removing
those volumes is an explicit separate choice. Image versions are fixed in
`compose.yml`; the app's multi-stage build uses the project's Boost pin and
builds without GoogleTest. Initial builds/pulls require network access. This is
a local development demonstration; public access/authentication and host CPU/RSS
exporters are not bundled. Container hostname resolution retains the
previously documented OS resolver cancellation limitation.

## Metric semantics

| Family | Labels / meaning |
|---|---|
| `lb_requests_total` | method: GET/POST/HEAD/PUT/DELETE/PATCH/OPTIONS/OTHER; status_class: 1xx..5xx; only successfully completed downstream writes |
| `lb_request_duration_seconds` | route=proxy only; first header byte received to completed write, or start of processing an already buffered pipeline request; no keep-alive idle time |
| `lb_inflight_requests` | Admitted requests from valid headers through response completion/cancellation; excludes rejected/unparsed requests |
| `lb_active_connections` | side=downstream/upstream; established application TCP sockets, upstream includes probes, monitoring sockets excluded |
| `lb_backend_healthy` | Backend belongs to the current eligible snapshot |
| `lb_backend_requests_total` | backend, result=success/http_error/transport_error/timeout/cancelled; finalized upstream attempts; HTTP >=400 is http_error, an HTTP 3xx is success |
| `lb_upstream_errors_total` | backend, reason=resolve/connect/timeout/reset/protocol/transport; excludes HTTP status errors and cancellation |
| `lb_healthcheck_total` | backend, result=success/failure; accepted readiness results only, failure includes HTTP/transport/timeout |
| `lb_backend_state_changes_total` | backend, to_state=unknown/healthy/unhealthy/draining; actual published changes including UNKNOWN→HEALTHY bootstrap, no increment on unchanged state |
| `lb_overload_rejections_total` | reason=connection_limit/request_limit/upstream_limit/shutdown; each rejection has one reason, no rate limiter |

Histogram finite upper bounds in seconds: 0.001, 0.005, 0.01, 0.025, 0.05, 0.1,
0.25, 0.5, 1, 5, then +Inf. Buckets are cumulative, +Inf equals count, and each
scrape snapshots response counts/buckets/sum under a short lock. Other gauges
and backend atomics are sampled independently, so a scrape is not a global
transaction across all runtime events. Unfinished writes do not increment
response counters or histograms. Backend attempts can complete before their
downstream response, so those counts need not match at every instant.

All fixed combinations are present even at zero. At N configured backends there
are 60+18N series (at most 1,212 for CLI maximum N=64). No raw path, request ID,
client address, or error text becomes a label. Backend identities are immutable;
replacing/removing an identity through the registry API discards that object's
exported counters. Runtime hot reload is not exposed by the current CLI.

The monitoring listener has an independent cap of eight sessions, an 8 KiB
header limit, no request bodies, five-second read/write deadlines, GET /metrics
only, and no keep-alive. The ninth accepted monitoring socket closes promptly.
Shutdown cancels all monitoring I/O immediately. These sockets and their one
listener/transient accept add to the business/probe resource bound.

## Containers healthy but browser connection refused

A container can answer its own health check while its host port is unavailable.
The earlier single-network configuration attached every service exclusively to
`internal: true`; affected Docker Engine/Desktop versions silently omit host
port mappings in that case. The corrected configuration attaches the proxy,
Prometheus, and Grafana to both the internal monitoring network and a normal
bridge named `host-access`. Published ports remain bound to `127.0.0.1`.

Apply this configuration to existing containers without deleting data volumes:

```sh
python3 tests/integration/monitoring_config.py &&
docker compose up -d --force-recreate --wait --wait-timeout 120 &&
python3 tests/integration/monitoring_stack.py
```

The config check rejects published services attached only to internal networks.
The runtime check first verifies host access to Prometheus and Grafana before
checking scrape data and the dashboard. If a connection still fails, collect:

```sh
docker compose ps
docker compose logs --tail 50 prometheus grafana
```

See the [Docker Engine report and workaround](https://github.com/moby/moby/discussions/53256).
