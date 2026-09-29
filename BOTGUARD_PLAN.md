# BotGuard MVP Plan

## 1. Product hypothesis

**BotGuard is a local execution-safety layer between an automated trading strategy and an exchange API.**

BotGuard does not predict prices and does not generate trading signals. Its job is to prevent unsafe order execution caused by:

- software bugs;
- duplicate retries;
- timeout / ambiguous transport outcomes;
- stale market or account state;
- inconsistent restart state;
- risk-limit violations;
- kill-switch activation;
- failed persistence;
- failed reconciliation.

The initial beachhead is API-driven retail/prosumer trading bots, with prediction markets as the first integration target.

The domain/risk core must remain venue-agnostic.

---

## 2. Product invariant

A strategy must not be able to bypass BotGuard's authoritative execution sequence.

The production path is:

```text
Strategy
    |
    | SubmitIntent
    v
BotGuard local agent
    |
    +--> venue preflight
    +--> authoritative market/account state
    +--> risk evaluation
    +--> local reservation
    +--> durable PENDING_SUBMIT
    +--> external submit
    +--> lifecycle update
    +--> durable final/ambiguous state
    +--> reconciliation after restart/failure
    |
    v
Exchange
```

The strategy must not perform the real exchange submit itself after receiving an `ALLOW`.

This avoids a TOCTOU gap between risk approval and execution.

---

## 3. V0 safety rules

The V0 risk/execution layer enforces:

1. Invalid price guard.
2. Invalid quantity guard.
3. Duplicate `client_order_id` guard.
4. Stale market-data protection.
5. Market-data trust boundary: freshness evidence is owned by BotGuard, not the strategy.
6. Authoritative account-state freshness protection.
7. Authoritative account-state version regression protection.
8. Maximum order notional.
9. Maximum gross exposure per market.
10. Maximum total gross exposure.
11. Maximum loss since the durable baseline.
12. Manual global kill switch.
13. Durable `PENDING_SUBMIT` barrier before any external side effect.
14. `UNKNOWN` handling for ambiguous submit outcomes.
15. Restart-safe lifecycle recovery.
16. Exchange-state reconciliation.
17. Fail-closed handling of unsupported partial-fill states.
18. Durable per-UTC-day equity baseline established on first safe observation.
19. Pre-submit revalidation immediately before the external side effect.
20. Single-process runtime lease for each storage/account owner.
21. Shared storage generation across order and runtime databases.
22. Complete positions-page evidence, including the required pagination cursor.
23. Durable reservation validation against the persisted economic identity.

Every rejection or pre-submit abort must expose a machine-readable reason.

---

## 4. Non-goals for V0

Do not build yet:

- prediction models;
- AI trading signals;
- portfolio optimisation;
- automated arbitrage discovery;
- frontend SaaS;
- billing;
- cloud custody of exchange credentials;
- event dependency graph;
- cross-market correlation engine;
- multi-exchange architecture;
- HFT-specific kernel/network tuning;
- automatic hedging;
- complex partial-fill accounting;
- cloud dashboard.

Correctness, determinism and recoverability take priority over optimisation.

---

## 5. Technology and implementation constraints

- C++23.
- CMake.
- GoogleTest.
- Linux as the primary production target.
- `.cc` source files.
- `.h` headers.
- Header and implementation colocated inside the feature/module directory.
- Namespace pattern: `botguard::<module>`.
- RAII.
- Strong/domain types where useful.
- Fixed-point integer arithmetic for money, price and quantity.
- No `double` in the risk core.
- `std::chrono::steady_clock` for elapsed time/freshness.
- No unnecessary heap allocation, I/O, logging or mutexes in the critical risk path.
- One owning execution/risk thread in V0.
- Global kill switch may remain atomic.
- Measure performance with benchmarks before optimisation.

---

## 6. Current architecture

```text
src/
├── agent/
│   ├── uds_protocol
│   ├── uds_agent_server
│   └── uds_client
│
├── demo/
│   ├── persistent_fault_injecting_venue
│   └── simulator_runtime
│
├── risk/
│   ├── types
│   ├── order_notional
│   ├── risk_engine
│   ├── order_gate
│   └── order_registry
│
├── execution/
│   ├── order_submission_coordinator
│   ├── reconciliation_coordinator
│   ├── startup_recovery
│   ├── order_state_store
│   ├── sqlite_order_state_store
│   ├── sqlite_runtime_state_store
│   ├── sqlite_storage_pair
│   ├── storage_generation
│   ├── runtime_lease
│   ├── authoritative_account_state_provider
│   ├── market_data_freshness_provider
│   ├── venue_order_query
│   ├── venue_startup_reconciler
│   ├── kalshi_adapter
│   ├── kalshi_startup_reconciler
│   └── kalshi/
│       ├── codec
│       └── signed_http_client
│
└── app/
    ├── demo
    └── kalshi_demo
```

Dependency rule:

```text
risk core
   ^
   |
execution core
   ^
   |
venue adapter / SQLite implementation / application
```

Exchange-specific types must not leak into `risk`.

---

## 7. Core domain model

### OrderIntent

```text
strategy_id
client_order_id
market_id
side
price
quantity
market_data_received_at
```

`market_data_received_at` remains useful for deterministic pure-core tests.

Production execution must replace it with BotGuard-owned `MarketDataFreshnessProvider` evidence.

### AccountState

```text
market_gross_exposure
total_gross_exposure
pnl_since_baseline
```

This is authoritative exchange/account exposure only.

Local unresolved reservations are added separately by `OrderGate`.

### RiskLimits

```text
max_order_notional
max_market_gross_exposure
max_total_gross_exposure
max_loss_since_baseline
max_market_data_age
```

### RiskDecision

```text
ALLOW / REJECT
order_notional
one or more RejectReason values
```

### Durable order identity

Persistent recovery state includes:

```text
strategy_id
client_order_id
market_id
side
price
quantity
lifecycle state
reserved_notional
reservation_active
optional venue_order_id
```

The complete original economic intent is preserved for reconciliation.

---

## 8. Lifecycle model

Current states:

```text
PENDING_SUBMIT
OPEN
FILLED
CANCELLED
REJECTED
ABORTED_BEFORE_SUBMIT
UNKNOWN
```

Safety rules:

- `PENDING_SUBMIT`, `OPEN`, `UNKNOWN` retain reservation.
- `FILLED` retains reservation until authoritative account exposure is known to contain the fill.
- `CANCELLED`, `REJECTED`, `ABORTED_BEFORE_SUBMIT` release reservation.
- terminal identity remains claimed permanently.
- `client_order_id` is the idempotency identity.
- price/quantity equality is never used as a substitute for intent identity.

---

## 9. Durability model

Before `Submit()`:

```text
risk ALLOW
-> reserve
-> write PENDING_SUBMIT
-> durable commit
-> revalidate safety
-> Submit()
```

If the first durable write fails and rollback is proven:

```text
remove in-memory reservation
release client_order_id
do not submit
```

If the external side effect may already have happened:

```text
never roll back identity
never blindly retry
require reconciliation
```

SQLite currently uses a fail-stop policy when commit outcome cannot be proven.

---

## 10. Startup recovery model

Production startup sequence:

```text
load durable order state
-> restore OrderRegistry
-> reconcile PENDING_SUBMIT / UNKNOWN
-> persist reconciliation results
-> refresh authoritative account state
-> validate account-state cursor/freshness
-> create StartupRecoverySession
-> enable trading capability
```

No successful `StartupRecoverySession` means no order admission.

A successful adapter return alone is not sufficient; `ReconciliationCoordinator` must remain healthy.

---

## 11. Kalshi V0 adapter

Current first venue: **Kalshi**.

Implemented responsibilities:

- signed REST transport;
- RSA-PSS request signing;
- submit limit order;
- venue order acknowledgement;
- lookup by `venue_order_id`;
- validation of original price/quantity/client identity;
- account-state refresh;
- market-data freshness refresh;
- startup reconciliation;
- subaccount binding;
- durable binding of the canonical `market_id` to ticker mapping;
- fail-closed handling of ambiguous response classes;
- partial-cancel fail-closed behavior.

Current V0 restriction:

```text
BUY / YES-side only
```

`SELL` is rejected because the current risk model does not yet reserve opposite-outcome liability correctly.

---

## 12. Current implementation status

### Phase 0 — deterministic risk core

**Status: DONE**

Delivered:

- fixed-point domain types;
- `RiskEngine`;
- checked notional arithmetic;
- risk reason codes;
- kill switch;
- deterministic tests;
- CLI safety demo.

### Phase 1 — lifecycle / idempotency / persistence

**Status: DONE**

Delivered:

- `OrderRegistry`;
- local reservations;
- lifecycle state machine;
- duplicate intent protection;
- durable `PENDING_SUBMIT`;
- SQLite recovery store;
- `UNKNOWN`;
- restart recovery;
- persistence failure handling;
- reconciliation coordinator;
- process/restart integration scenarios.

### Phase 2 — first venue adapter

**Status: MOSTLY DONE**

Delivered:

- Kalshi adapter;
- signed HTTP client;
- order submission;
- venue-order lookup;
- authoritative account state;
- BotGuard-owned market-data freshness;
- Kalshi startup reconciler;
- partial-fill fail-closed protection;
- loopback integration tests.

Remaining before considering the adapter production-ready:

- real Kalshi Demo validation;
- characterization of duplicate `client_order_id` behavior;
- crash-point testing against real Demo API;
- bootstrap proof that the isolated Demo subaccount has no untracked resting orders.

Real Kalshi validation is an external compatibility track, not a blocker for
the local product-safety milestones.

---

## 13. Immediate next tasks

Pre-Demo storage hardening is complete: runtime/account/market binding, a
required dedicated numbered subaccount, SQLite failure tests, single-process
lease, shared storage generation, orphan SQLite-sidecar detection, required
positions cursor, and restored-reservation validation are implemented.

### 13.1 Local failure-simulator vertical slice

**Status: DONE**

The standalone `botguard_failure_demo` proves, with real SQLite files and
`SIGKILL` process termination:

```text
accepted venue order + lost response
-> durable UNKNOWN
-> restart
-> venue reconciliation to OPEN
-> repeated client_order_id rejected
-> venue submit count remains one
```

Automated crash points:

```text
AFTER_PENDING_DURABLE
BEFORE_HTTP
AFTER_HTTP_RETURN
BEFORE_FINAL_PERSIST
```

The demo also proves the fail-closed branch:

```text
accepted venue order + UNKNOWN
-> restart
-> reconciliation unavailable
-> no StartupRecoverySession
-> no trading capability
-> no second submit
```

The simulator implements the venue-neutral boundaries directly. It does not
claim that Kalshi can recover `UNKNOWN` without a `venue_order_id`; that case
remains intentionally fail closed until real venue behavior is characterized.

### 13.2 Real Kalshi compatibility E2E

Run against a real Demo account:

```text
clean DB
-> status
-> tiny BUY
-> verify order exists on Kalshi
-> stop BotGuard
-> restart with same DB
-> recovery
-> repeat same client_order_id
-> verify no second submit occurs
```

This remains important compatibility validation, but no longer blocks local
product development.

### 13.3 Kalshi client_order_id characterization

Experiment with:

```text
POST client_order_id = X
-> accepted

repeat identical POST client_order_id = X
-> record exact status/body/semantics
```

Check separately for orders that became:

```text
resting
filled
cancelled
```

Determine whether the venue can safely recover an `UNKNOWN` order when `venue_order_id` was never received.

Do not infer semantics from documentation alone; record the actual Demo behavior.

### 13.4 Optional crash-point testing against real Demo

Test process termination around:

```text
before durable PENDING
after durable PENDING
during HTTP submit
after exchange acceptance
after response receipt
before final SQLite write
after final SQLite write
```

Success criterion:

> No crash point permits a blind duplicate economic submit.

---

## 14. Safe FILLED reservation release

**Status: DEFERRED pending causal inclusion proof**

Current behavior intentionally double-counts some exposure:

```text
FILLED local reservation
+
authoritative account position
```

This is safe but may reduce available capacity.

Before automatically calling `MarkExposureReconciled()`, BotGuard must prove:

> The authoritative account snapshot is causally new enough to include the specific fill.

Do not release a FILLED reservation solely because the lifecycle state is `FILLED`.

Possible future evidence:

```text
fill observation/version/time
+
account snapshot revision/time
+
venue consistency guarantees
```

Until proven safe, keep the conservative reservation.

---

## 15. Venue preflight

**Status: DONE**

Before creating durable `PENDING_SUBMIT`, validate venue-specific representability.

Kalshi V0 preflight:

```text
market is bound
side == BUY
price is representable
quantity is representable
```

Implemented sequence:

```text
SubmitIntent
-> venue preflight
-> authoritative safety inputs
-> risk
-> reserve
-> durable PENDING_SUBMIT
-> pre-submit revalidation
-> exchange submit
```

This avoids durable pending records for orders that the adapter would reject locally without making an HTTP request.

---

## 16. Phase 3 — local execution agent

**Status: SUBMITINTENT + STATUS + DURABLE KILL + CONTROLLED RESUME IMPLEMENTED**

Delivered:

- Linux `AF_UNIX/SOCK_SEQPACKET` transport;
- bounded versioned wire codec with explicit stable wire enums;
- economic intent only on the request wire;
- machine-readable submission status/outcome/notional/reject mask;
- single execution thread;
- persistent simulator runtime shared with the failure demo;
- stale socket recovery while holding the runtime lease;
- process-level response-loss/retry/`SIGKILL`/restart test;
- minimal dependency-free Python client;
- generic request dispatcher;
- read-only `GetStatus` with no venue I/O or state transition;
- durable idempotent `Kill` serialized on the execution thread;
- process-level proof that post-kill intent does not reach venue submit;
- live `RECOVERY_REQUIRED` control-plane state;
- candidate recovery context published only after complete recovery;
- controlled `Resume` with a distinct machine-readable result;
- process-level kill/recovery/resume/crash matrix;
- live gate degradation reflected as `RECOVERY_REQUIRED` by the control plane;
- one bounded C++ and Python client deadline across connect/send/receive;
- bounded nonblocking accepted-peer I/O on the single-threaded UDS server,
  with independent receive and response-send quotas;
- per-admission authoritative refresh and controlled UTC-day rollover in the
  persistent simulator;
- process-level `SIGKILL` proof after durable baseline rotation.

Only the operator admission latch is durable. `READY` and
`RECOVERY_REQUIRED` are never stored: they are derived from the latch, the
in-memory kill switch, and the presence of a freshly recovered capability.

Next control-plane sequence:

```text
long-running real-adapter composition
-> OPEN-order liveness reconciliation
```

The agent must **own order submission**.

Do not build the authoritative production path as:

```text
POST /risk/check
-> ALLOW
-> strategy submits directly
```

Instead:

```text
Strategy
    |
    v
SubmitIntent over local IPC
    |
    v
BotGuard execution thread
    |
    v
Kalshi
```

### Initial transport

Primary Linux V0 transport:

```text
Unix Domain Socket
```

Avoid HTTP on the order hot path unless there is a concrete need.

### Minimal commands

```text
SubmitIntent
GetStatus
Kill
Resume
```

`Resume` must not directly flip the gate to READY.

---

## 17. Phase 4 — Python client

**Status: MINIMAL SUBMITINTENT/STATUS/KILL/RESUME CLIENT DONE**

Build a small Python client after the local agent exists.

Target usage:

```python
client = BotGuardClient("/run/botguard.sock")

result = client.submit(
    client_order_id=...,
    market_id=...,
    side="buy",
    price=...,
    quantity=...,
)
```

The Python client should expose:

- typed/simple request objects;
- machine-readable rejection reasons;
- lifecycle/ambiguous result;
- health/status;
- kill switch.

Deployment invariant:

> The trading strategy should not need direct exchange credentials.

Exchange API keys remain in the local BotGuard agent.

---

## 18. Phase 5 — long-running UTC-day rollover

The current CLI resolves the per-UTC-day baseline during process startup. V0
does not claim that this value is historical equity at exactly `00:00:00 UTC`;
Kalshi exposes current equity but no documented historical portfolio-value
primitive. Risk therefore enforces loss since the durable baseline.

A daemon can cross UTC midnight without restarting, so it needs a controlled rollover.

Required sequence:

```text
detect UTC day change
-> stop admission
-> finish/fail-close active execution boundary
-> refresh authoritative equity
-> durably rotate equity baseline from the first safe authoritative observation
-> refresh authoritative account state
-> establish new account-state baseline
-> resume
```

Do not rotate the baseline while an unresolved submission transition is in progress.

---

## 19. Phase 6 — auditability

Implemented in order-state schema version 4. The latest recovery state remains
in `order_state`; causal lifecycle history is stored in append-only
`order_event` in the same SQLite database:

```text
order_event
-----------
sequence
client_order_id
event_type
resulting_state
venue_order_id
```

Rules:

- a state transition and its event commit in one `BEGIN IMMEDIATE` transaction;
- SQLite-assigned `sequence` is the causal order; correctness does not depend on wall-clock time;
- retrying the same semantic transition is idempotent only when immutable
  identity and reserved notional match the durable row;
- `VENUE_FILLED` and `RECONCILED_FILLED` require an active reservation;
  `EXPOSURE_RECONCILED` requires a released reservation;
- production code has no `UPDATE`/`DELETE` operation for `order_event`;
- schema validation rejects triggers on safety tables;
- version-3 order storage fails closed because missing history cannot be fabricated.

V0 events:

```text
INTENT_PENDING_DURABLE
ABORTED_BEFORE_SUBMIT
VENUE_OPEN
VENUE_FILLED
VENUE_REJECTED
VENUE_UNKNOWN
RECONCILED_OPEN
RECONCILED_FILLED
RECONCILED_CANCELLED
RECONCILED_REJECTED
EXPOSURE_RECONCILED
```

Risk rejections are deliberately excluded because they do not change durable
order lifecycle. Hash chains, signatures, remote shipping, and WORM storage are
outside the V0 crash-consistent local-history threat model.

Status: **DONE**. Atomicity, event-insert rollback, immutable-identity retry,
FILLED reservation-phase, crash-prefix, and restart/reconciliation history are
covered by tests.

---

## 20. Phase 7 — control plane

After the execution agent is stable, expose operational controls.

Potential local endpoints/commands:

```text
status
health
kill
resume
risk snapshot
```

`resume` means:

```text
stop admission
-> reconcile unresolved orders
-> refresh authoritative account state
-> validate runtime state
-> create a new trading capability
-> READY
```

It must never mean:

```text
gate = READY
```

---

## 21. Phase 8 — public beta

Before opening the repository for external testers:

- clean public GitHub repository;
- LICENSE;
- concise README;
- architecture diagram;
- threat/failure model;
- 5-minute local demo;
- Kalshi Demo setup guide;
- secret-handling instructions;
- reproducible timeout/UNKNOWN demo;
- Linux build instructions;
- test status;
- explicit V0 limitations;
- contribution guide;
- security reporting instructions.

Primary message:

> Exchange timed out. BotGuard prevents your retry from becoming an unintended duplicate order.

Do not lead with C++23 or latency.

---

## 22. First-user validation

Target first users:

- developers running real prediction-market bots;
- Python/TypeScript bot authors;
- developers who have experienced timeout/retry/reconciliation failures.

Initial goal:

```text
5 developers open the repo
2 run the demo
1 integrates BotGuard into a real bot
```

Then expand toward:

```text
5–10 active design partners
```

Do not optimise for GitHub stars.

Track:

- successful installs;
- time-to-first-safe-submit;
- safety events caught;
- number of UNKNOWN/reconciliation events;
- successful restart recoveries;
- percentage of bot orderflow routed exclusively through BotGuard;
- design-partner retention.

---

## 23. Observability

Only after the execution path is trusted, add:

- structured local audit events;
- health counters;
- rejection reason statistics;
- recovery failures;
- persistence failures;
- UNKNOWN count;
- kill-switch state;
- protected/rejected notional;
- decision latency benchmarks;
- optional Telegram/Slack alerts.

Alerts must not be required for correctness.

---

## 24. Future product differentiation

Only after real usage validates the core product:

- correlated event exposure;
- cross-market dependencies;
- settlement-aware risk;
- cross-venue exposure;
- scenario/worst-case P&L;
- policy templates;
- fleet management;
- remote read-only telemetry;
- commercial control plane;
- second exchange adapter.

Do not build multi-exchange architecture before users require it.

---

## 25. Security model

Target deployment:

```text
User strategy
    |
    | local IPC
    v
BotGuard local agent
    |
    | exchange credentials remain here
    v
Exchange
```

Rules:

- API keys stay on the user's machine/VPS.
- The strategy should not need the exchange private key.
- Cloud services, if added later, receive only explicitly enabled telemetry.
- Private keys are never persisted in the risk core.
- Telegram/Slack must never receive exchange credentials.
- Local state databases must be bound to the expected venue/account runtime.

---

## 26. Performance policy

BotGuard is latency-sensitive, but V0 is not an HFT engine.

Critical invariant:

`RiskEngine::Evaluate()` performs no:

- I/O;
- logging;
- heap allocation;
- mutex acquisition.

Performance work must be driven by benchmarks.

For prediction-market / retail-algo workloads, BotGuard overhead should be negligible relative to exchange/network latency.

If future use cases need lower latency, the same core may support an in-process integration path.

---

## 27. Technical debt explicitly tracked

Known debt / deferred work:

1. Safe release of FILLED reservations only after a causal inclusion proof exists.
2. Full partial-fill lifecycle/accounting.
3. UNKNOWN recovery without `venue_order_id`, pending Kalshi characterization.
4. Explicit operator-verified storage migration for API-key rotation.
5. Long-running UTC-day rollover.
6. OPEN-order liveness reconciliation.
7. Checked/safer construction helpers for extreme fixed-point factory inputs.
8. `noexcept` + container allocation policy in some recovery paths.
9. Second venue adapter only after real demand.

Do not hide these with optimistic comments or silent assumptions.

---

## 28. Exit criteria by milestone

### Core safety

Complete when:

- rule boundaries are deterministic;
- all risk reasons are test-covered;
- no caller-supplied account state enters the production execution path;
- no caller-supplied market-data freshness enters the production execution path.

### Persistence/recovery

Complete when:

- durable pending exists before submit;
- restart restores identity and reservations;
- ambiguous orders cannot be blindly retried;
- failed reconciliation prevents trading capability creation.

### Kalshi Demo validation

Complete when:

- a real Demo order passes through the complete BotGuard stack;
- restart succeeds safely;
- duplicate retry is blocked;
- crash-point tests cannot create duplicate economic orders;
- real `client_order_id` semantics are documented.

### Local agent

Complete when:

- a sample Python bot routes order submission exclusively through BotGuard;
- the bot does not directly submit to Kalshi;
- kill/recovery behavior works through IPC.

### Public beta

Complete when:

- a new user can build/run the demo in under ~5 minutes;
- at least one external developer successfully integrates the local agent;
- limitations and failure semantics are documented.

---

## 29. Immediate execution order

Do work in this order unless simulator or real Kalshi behavior forces a change:

```text
1. Keep failure simulator and process-level UDS kill/recovery/resume/crash/timeout/rollover matrix green.
2. Compose the long-running safety sequence with the real adapter through an explicit trading-day context operation.
3. Keep atomic append-only audit and crash/reconciliation history tests green.
4. Add OPEN-order liveness reconciliation.
5. Run real Kalshi Demo E2E as an external compatibility test.
6. Characterize Kalshi duplicate client_order_id behavior.
7. Add authoritative bootstrap verification of resting venue orders.
8. Revisit FILLED reservation release only with causal inclusion evidence.
9. Prepare public GitHub beta.
```

---

## 30. Product kill / pivot criteria

Stop or pivot if, after a usable local prototype and direct testing with active algo traders:

1. Existing user risk layers already solve timeout/idempotency/recovery adequately.
2. Real users do not want BotGuard to own their exchange submission path.
3. Fewer than roughly 5 of 20 qualified testers see enough value to keep using it.
4. Exchange integration maintenance dominates product value.
5. Users only value trivial max-position/max-loss checks.
6. Platform or regulatory constraints make deployment impractical.
7. The safety guarantees cannot be made reliable enough under real venue behavior.

---

## 31. Commercial target

Do not optimise for mass adoption initially.

A plausible target is a small number of serious users whose protected capital or operational risk materially exceeds the product cost.

Possible future model, to validate later:

```text
local/basic          free or low-cost
individual trader    ~$39–79/month
multi-strategy/pro   ~$149–299/month
small team           higher if audit/fleet controls justify it
```

Do not build billing or cloud infrastructure until users validate the execution-safety product itself.
