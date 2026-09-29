# BotGuard

Local venue-agnostic execution-safety layer for automated trading bots.

See [`BOTGUARD_PLAN.md`](BOTGUARD_PLAN.md) for the product and implementation plan.

## Layout

Headers live next to their implementations by feature:

```text
src/
├── app/
│   └── main.cpp
└── risk/
    ├── types.hpp
    ├── risk_engine.hpp
    └── risk_engine.cpp

tests/
└── risk/
    └── risk_engine_tests.cpp
```

We intentionally do not maintain a separate global `include/` tree. Public/private packaging can be revisited if BotGuard becomes a distributable library.

## Latency stance

BotGuard is latency-sensitive, but V0 is not an HFT system.

The pre-trade core is designed so that `RiskEngine::Evaluate()` performs:

- no I/O;
- no logging;
- no heap allocation;
- no mutex acquisition;
- fixed-point arithmetic for monetary values;
- monotonic-time freshness checks.

Initial engineering target: keep local risk evaluation comfortably below network/exchange latency. We will add a benchmark before introducing IPC and enforce a p99 budget from measured data rather than guessing.

If a future venue/use-case genuinely requires sub-millisecond end-to-end overhead, BotGuard can expose the same core as an in-process library instead of routing the hot path through HTTP.

## Build core/demo

```bash
cmake -S . -B build -DBOTGUARD_BUILD_TESTS=OFF
cmake --build build
./build/botguard_demo
```

## Build tests

GoogleTest must be installed and discoverable by CMake.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Primary failure-simulator demo

The primary demo needs no exchange account or credentials:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBOTGUARD_BUILD_KALSHI_ADAPTER=OFF \
  -DBOTGUARD_BUILD_TESTS=OFF
cmake --build build
./build/botguard_failure_demo
```

It uses real SQLite files, a persistently stateful fault-injecting venue, real
process termination through `SIGKILL`, restart reconciliation, and duplicate
intent retry. Its main scenario accepts an order at the venue but loses the
submit response. After restart BotGuard finds the order through reconciliation
and rejects the repeated `client_order_id` without a second venue submission.
The fake venue implements the venue-neutral submission/query boundaries
directly; it is not an HTTP server and does not claim to reproduce Kalshi's
behavior for `UNKNOWN` orders without an acknowledgement.

The executable also runs this crash matrix:

```text
scenario=ACCEPTED_BUT_RESPONSE_LOST
  venue_submit_count=1
  recovered_state=OPEN
  retry_result=DUPLICATE_CLIENT_ORDER_ID
  second_submit=false
  result=PASS
scenario=RECONCILIATION_UNAVAILABLE
  venue_submit_count=1
  recovery=FAIL_CLOSED
  trading_capability=ABSENT
  second_submit=false
  result=PASS
scenario=AFTER_PENDING_DURABLE
  venue_submit_count=0
  recovered_state=REJECTED
  retry_result=DUPLICATE_CLIENT_ORDER_ID
  second_submit=false
  result=PASS
scenario=BEFORE_HTTP
  venue_submit_count=0
  recovered_state=REJECTED
  retry_result=DUPLICATE_CLIENT_ORDER_ID
  second_submit=false
  result=PASS
scenario=AFTER_HTTP_RETURN
  venue_submit_count=1
  recovered_state=OPEN
  retry_result=DUPLICATE_CLIENT_ORDER_ID
  second_submit=false
  result=PASS
scenario=BEFORE_FINAL_PERSIST
  venue_submit_count=1
  recovered_state=OPEN
  retry_result=DUPLICATE_CLIENT_ORDER_ID
  second_submit=false
  result=PASS
```

This executable is also registered with CTest. The Kalshi Demo below remains
an external compatibility test and is not required to reproduce BotGuard's
core safety proof.

## Local UDS agent

`botguard_local_agent` exposes `SubmitIntent`, read-only `GetStatus`, durable
idempotent `Kill`, and controlled `Resume` commands over Linux
`AF_UNIX/SOCK_SEQPACKET`. Frames have explicit magic, protocol version, message
type and fixed-width big-endian fields. The strategy sends only economic
intent; it cannot provide market-data timestamps, account state, risk results
or venue fields.

Start the simulator-backed agent:

```bash
./build/botguard_local_agent \
  /tmp/botguard.sock \
  ./agent-orders.db \
  ./simulated-venue.db \
  --lose-submit-response
```

Submit from another terminal with the dependency-free Python client. Price is
in micros per unit and quantity is in microunits:

```bash
python3 clients/python/botguard_client.py \
  submit /tmp/botguard.sock 1 1001 10 buy 250000 4000000

# Retry the same economic identity.
python3 clients/python/botguard_client.py \
  submit /tmp/botguard.sock 1 1001 10 buy 250000 4000000

python3 clients/python/botguard_client.py status /tmp/botguard.sock
python3 clients/python/botguard_client.py kill /tmp/botguard.sock
python3 clients/python/botguard_client.py resume /tmp/botguard.sock
```

Expected responses:

```text
{'status': 'SUBMITTED', 'submit_outcome': 'UNKNOWN', ...}
{'status': 'RISK_REJECTED', ..., 'reject_reasons': ('DUPLICATE_CLIENT_ORDER_ID',)}
```

The process-level integration test additionally covers response loss,
unavailable reconciliation, live `RECOVERY_REQUIRED`, failed and successful
`Resume`, SQLite-busy during the final resume commit, durable `Kill`, multiple
`SIGKILL`/restart cycles, and proves the persistent venue submit count remains
one:

```bash
./build/botguard_uds_tests
```

The socket is mode `0600` and the server is deliberately single-threaded.
Accepted peer I/O is nonblocking and bounded by a two-second end-to-end
deadline by default, so a client that connects without sending a frame cannot
hold the execution/control thread indefinitely. The deadline is configurable
through `UdsServerOptions`.
`GetStatus` performs no venue refresh or reconciliation. `Kill` prevents new
submissions and acknowledges success only after the operator latch is durable.
The database never stores `READY`: every process start and every `Resume` must
construct a fresh successful recovery capability before admission opens. A
failed recovery leaves the UDS control plane alive in `RECOVERY_REQUIRED`, with
submission and the in-memory kill switch closed.

The C++ client uses a nonblocking socket and one end-to-end deadline shared by
connect, send, and receive. Every operation defaults to five seconds and can be
bounded explicitly with `UdsClientOptions{.timeout = ...}`. Timeout does not
imply that an intent was not processed; retries must keep the same
`client_order_id`.

The Python client enforces the same single end-to-end deadline, uses one
`send()` per `SOCK_SEQPACKET` request, and rejects non-zero reserved response
bytes. Its timeout can be selected with `BotGuardClient(path, timeout=...)`.

The runtime-state schema is now version 4. Older prototype `.runtime` files do
not silently migrate or open; they fail closed and require an explicit
operator-controlled migration or a fresh dedicated demo storage pair.

The order-state schema is also version 4 and contains an append-only
`order_event` stream beside the latest `order_state`. Every durable lifecycle
change is persisted through one `BEGIN IMMEDIATE` transaction that updates the
recovery state and inserts its matching event before `COMMIT`. SQLite assigns a
monotonically increasing `sequence`; causal ordering does not depend on a wall
clock. Retrying the same semantic transition is idempotent only when its
immutable order identity and reserved notional match the durable row; a
mismatch fails instead of returning a false durability acknowledgement. A
valid retry cannot append a duplicate event or regress a later state. FILLED
events also encode the reservation phase: venue/reconciliation FILLED requires
an active reservation, while `EXPOSURE_RECONCILED` requires its release.
Production schema validation rejects audit triggers, and production code has
no update/delete path for events.
Version-3 order databases fail closed because historical events that predate
this invariant cannot be reconstructed safely.

The long-running simulator checks the current UTC day and refreshes
authoritative equity on every admission. A day change first closes admission,
then fixes a new baseline from an authoritative equity observation, refreshes
account state against that context, performs fresh recovery, and only then
returns to `READY`. Persistence/refresh failure, an unavailable clock, or day
regression leaves the runtime in `RECOVERY_REQUIRED` without a venue submit.
The resulting risk metric is explicitly `pnl_since_baseline`: the venue does
not provide historical equity at the UTC boundary, so BotGuard does not claim
calendar-day PnL. Activity between midnight and the first safe observation is
outside this V0 loss metric.
`SimulatorRuntimeSafetyInputs` makes the wall clock and equity source
deterministic in rollover and crash tests. Independently, every submission
coordinator binds to that active context and checks the UTC day both at
`Execute` entry and again immediately before venue `Submit`. A midnight change
after durable `PENDING_SUBMIT` becomes durable `ABORTED_BEFORE_SUBMIT` and
requires reconciliation; no venue call occurs.

## Kalshi Demo vertical slice

`botguard_kalshi_demo` is pinned to Kalshi's demo API. It owns the full
`SQLite load/restore -> venue-order reconciliation -> account refresh ->
BotGuard-owned market-data refresh -> risk -> persist -> submit -> persist`
path for one account and one market.
Credentials are read from environment variables; the private key itself is
never passed on the command line.

```bash
cmake -S . -B build-kalshi -DBOTGUARD_BUILD_KALSHI_ADAPTER=ON
cmake --build build-kalshi

export KALSHI_DEMO_API_KEY_ID='your-demo-key-id'
export KALSHI_DEMO_PRIVATE_KEY_PATH='/absolute/path/to/demo-private-key.pem'

# No-submit account refresh and startup recovery. On first/new-day bootstrap,
# baseline equity is observed authoritatively from Kalshi portfolio_value.
./build-kalshi/botguard_kalshi_demo status \
  ./kalshi-demo-state.db MARKET-TICKER 1 DEDICATED_SUBACCOUNT

# Submit one BUY limit order. Quantity is expressed in centicontracts:
# 100 = 1.00 contract. Use a never-before-used client order ID.
BOTGUARD_DEMO_CONFIRM_SUBMIT=YES \
./build-kalshi/botguard_kalshi_demo submit \
  ./kalshi-demo-state.db MARKET-TICKER 1 DEDICATED_SUBACCOUNT \
  CLIENT_ORDER_ID PRICE_CENTS QUANTITY_CENTICONTRACTS
```

Use a numbered Demo subaccount dedicated exclusively to BotGuard; do not use
the primary account or place manual/external orders in it. The demo uses
conservative fixed limits: $10 per order, $100 per market,
$250 total gross exposure, and a $25 maximum loss since the durable baseline.
It supports BUY only.
Keep the SQLite database permanently bound to the same Kalshi account and
market mapping. `DB_PATH`, `DB_PATH.runtime`, and the persistent
`DB_PATH.lock` name form one runtime storage set. The lock file carries no
state, but its OS lease prevents two BotGuard processes from owning the same
runtime concurrently. The two databases contain the same random storage
generation; a missing or substituted member fails closed. Bootstrap also
fails if either missing main database has an orphan `-wal`, `-shm`, or
`-journal` sidecar.

The adjacent `DB_PATH.runtime` database durably fixes the first safe,
authoritatively observed equity baseline for each UTC day. It is not asserted
to equal portfolio equity at exactly `00:00:00 UTC`. A same-day restart reuses that value;
a new UTC day observes current equity and atomically rotates it before venue
reconciliation and contextual account refresh. Its schema also stores a
SHA-256 binding over the demo venue,
base URL, API key ID, subaccount, and the `market_id` to ticker mapping.
Changing any of these values fails closed. API-key rotation therefore requires
an explicit, operator-verified runtime-state migration. Pre-generation SQLite
schemas are intentionally not accepted automatically. Preserve both databases.
If startup recovery fails, do not delete either database merely to make the
process start: investigate the durable state and re-establish the correct
UTC-day baseline first.

A long-running agent stops admission when it detects a UTC-day change and
durably rotates this baseline from the first safe authoritative observation
before resuming. The current short-lived demo resolves the day and baseline on
every process start.

## Microbenchmark

This is a local `RiskEngine::Evaluate()` regression benchmark, not an end-to-end latency measurement:

```bash
cmake -S . -B build-bench \
  -DBOTGUARD_BUILD_TESTS=OFF \
  -DBOTGUARD_BUILD_BENCHMARKS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench
./build-bench/botguard_risk_bench
```

Do not use one machine's `ns/evaluate` number as an SLA. The purpose is to detect hot-path regressions as rules are added.
