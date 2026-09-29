# BotGuard — Architectural Decisions

## ADR-001 — Language

C++23.

## ADR-002 — Source layout

Headers use `.h`, implementations use `.cc`.

Header and implementation files live together inside their feature directory.

Example:

src/risk/
  types.h
  risk_engine.h
  risk_engine.cc

## ADR-003 — Financial representation

Money, Price and Quantity use fixed-point integer representation.

Floating-point arithmetic is not used in the risk hot path.

## ADR-004 — Time

Elapsed time and market-data freshness use `std::chrono::steady_clock`.

## ADR-005 — Risk hot path

RiskEngine::Evaluate() must not perform:
- network I/O
- file I/O
- logging
- heap allocation unless justified
- blocking synchronization unless justified

Performance is measured, not assumed.

## ADR-006 — Order identity

Two identical BUY orders may both be legitimate.

Duplicate detection is therefore based on intent/order identity,
not on equality of market, price and quantity.

## ADR-007 — Unknown exchange state

If an order submission result is unknown because of timeout or connection loss,
BotGuard must not blindly resubmit the same intent.

The order enters UNKNOWN state and must be reconciled with the exchange.

## ADR-008 — Current product scope

BotGuard is safety/operations infrastructure.

It does not predict markets and does not decide whether a trading strategy is profitable.