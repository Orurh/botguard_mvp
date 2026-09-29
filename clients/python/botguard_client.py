#!/usr/bin/env python3
"""Minimal dependency-free BotGuard local-agent client for Linux."""

from __future__ import annotations

import argparse
import dataclasses
import math
import socket
import struct
import time
from typing import Optional

MAGIC = 0x42475544
VERSION = 1
SUBMIT_INTENT = 1
SUBMIT_RESPONSE = 2
GET_STATUS = 3
STATUS_RESPONSE = 4
KILL = 5
KILL_RESPONSE = 6
RESUME = 7
RESUME_RESPONSE = 8

REQUEST = struct.Struct(">IHHQQQB7xqq")
RESPONSE = struct.Struct(">IHHHBB4xqQ")
EMPTY_REQUEST = struct.Struct(">IHH")
STATUS_RESPONSE_FRAME = struct.Struct(">IHHBBB5xQQq")
RESUME_RESPONSE_FRAME = struct.Struct(">IHHB7xBBB5xQQq")

STATUS_NAMES = {
    1: "RISK_REJECTED",
    2: "VENUE_PREFLIGHT_REJECTED",
    3: "PRE_SUBMIT_PERSISTENCE_FAILED",
    4: "ABORTED_BEFORE_SUBMIT",
    5: "PRE_SUBMIT_ABORT_PERSISTENCE_FAILED",
    6: "SUBMITTED",
    7: "POST_SUBMIT_PERSISTENCE_FAILED",
    8: "REGISTRY_TRANSITION_FAILED",
    100: "INVALID_REQUEST",
    101: "INTERNAL_ERROR",
}

OUTCOME_NAMES = {0: None, 1: "OPEN", 2: "FILLED", 3: "REJECTED", 4: "UNKNOWN"}

REJECT_REASON_NAMES = [
    "KILL_SWITCH_ACTIVE",
    "INVALID_PRICE",
    "INVALID_QUANTITY",
    "DUPLICATE_CLIENT_ORDER_ID",
    "RECONCILIATION_REQUIRED",
    "ACCOUNT_STATE_UNAVAILABLE",
    "STALE_ACCOUNT_STATE",
    "ACCOUNT_STATE_VERSION_REGRESSED",
    "ACCOUNT_STATE_CHANGED_BEFORE_SUBMIT",
    "MARKET_DATA_UNAVAILABLE",
    "MARKET_DATA_CHANGED_BEFORE_SUBMIT",
    "STALE_MARKET_DATA",
    "NOTIONAL_OVERFLOW",
    "MAX_ORDER_NOTIONAL_EXCEEDED",
    "MAX_MARKET_EXPOSURE_EXCEEDED",
    "MAX_TOTAL_EXPOSURE_EXCEEDED",
    "MAX_LOSS_SINCE_BASELINE_EXCEEDED",
]

AGENT_STATE_NAMES = {1: "READY", 2: "KILLED", 3: "RECOVERY_REQUIRED"}
GATE_STATE_NAMES = {1: "READY", 2: "RECONCILIATION_REQUIRED"}
RESUME_RESULT_NAMES = {1: "READY", 2: "ALREADY_READY", 3: "RECOVERY_FAILED", 4: "PERSISTENCE_FAILED"}


@dataclasses.dataclass(frozen=True)
class SubmitResult:
    status: str
    submit_outcome: Optional[str]
    order_notional_micros: int
    reject_reasons: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class StatusResult:
    agent_state: str
    kill_switch_active: bool
    gate_state: str
    durable_order_count: int
    active_reservation_count: int
    total_reserved_exposure_micros: int


@dataclasses.dataclass(frozen=True)
class ResumeResult:
    result: str
    status: StatusResult


class BotGuardClient:
    def __init__(self, socket_path: str, timeout: float = 5.0) -> None:
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout must be finite and positive")
        self._socket_path = socket_path
        self._timeout = timeout

    def submit(
        self,
        *,
        strategy_id: int,
        client_order_id: int,
        market_id: int,
        side: str,
        price_micros: int,
        quantity_microunits: int,
    ) -> SubmitResult:
        # These are protocol-v1 wire values, not C++ enum ordinals.
        side_value = {"buy": 1, "sell": 2}.get(side.lower())
        if side_value is None:
            raise ValueError("side must be 'buy' or 'sell'")
        frame = REQUEST.pack(
            MAGIC,
            VERSION,
            SUBMIT_INTENT,
            strategy_id,
            client_order_id,
            market_id,
            side_value,
            price_micros,
            quantity_microunits,
        )
        response = self._exchange(frame, RESPONSE.size)
        self._require_zero_reserved(response, (slice(12, 16),))
        magic, version, message_type, status, outcome, reason_count, notional, reason_mask = RESPONSE.unpack(response)
        if magic != MAGIC or version != VERSION or message_type != SUBMIT_RESPONSE:
            raise RuntimeError("invalid BotGuard response header")
        if status not in STATUS_NAMES or outcome not in OUTCOME_NAMES:
            raise RuntimeError("invalid BotGuard response enum")
        reasons = tuple(name for bit, name in enumerate(REJECT_REASON_NAMES) if reason_mask & (1 << bit))
        if len(reasons) != reason_count or reason_mask >> len(REJECT_REASON_NAMES):
            raise RuntimeError("invalid BotGuard reject reason mask")
        return SubmitResult(STATUS_NAMES[status], OUTCOME_NAMES[outcome], notional, reasons)

    def get_status(self) -> StatusResult:
        return self._status_exchange(GET_STATUS, STATUS_RESPONSE)

    def kill(self) -> StatusResult:
        return self._status_exchange(KILL, KILL_RESPONSE)

    def resume(self) -> ResumeResult:
        frame = EMPTY_REQUEST.pack(MAGIC, VERSION, RESUME)
        response = self._exchange(frame, RESUME_RESPONSE_FRAME.size)
        self._require_zero_reserved(response, (slice(9, 16), slice(19, 24)))
        (
            magic,
            version,
            message_type,
            result,
            agent_state,
            kill_switch,
            gate_state,
            durable_orders,
            active_reservations,
            total_reserved,
        ) = RESUME_RESPONSE_FRAME.unpack(response)
        if magic != MAGIC or version != VERSION or message_type != RESUME_RESPONSE:
            raise RuntimeError("invalid BotGuard resume response header")
        if result not in RESUME_RESULT_NAMES:
            raise RuntimeError("invalid BotGuard resume result")
        status = self._make_status(
            agent_state, kill_switch, gate_state, durable_orders, active_reservations, total_reserved
        )
        return ResumeResult(RESUME_RESULT_NAMES[result], status)

    def _status_exchange(self, request_type: int, response_type: int) -> StatusResult:
        frame = EMPTY_REQUEST.pack(MAGIC, VERSION, request_type)
        response = self._exchange(frame, STATUS_RESPONSE_FRAME.size)
        self._require_zero_reserved(response, (slice(11, 16),))
        (
            magic,
            version,
            message_type,
            agent_state,
            kill_switch,
            gate_state,
            durable_orders,
            active_reservations,
            total_reserved,
        ) = STATUS_RESPONSE_FRAME.unpack(response)
        if magic != MAGIC or version != VERSION or message_type != response_type:
            raise RuntimeError("invalid BotGuard status response header")
        return self._make_status(
            agent_state, kill_switch, gate_state, durable_orders, active_reservations, total_reserved
        )

    @staticmethod
    def _make_status(
        agent_state: int,
        kill_switch: int,
        gate_state: int,
        durable_orders: int,
        active_reservations: int,
        total_reserved: int,
    ) -> StatusResult:
        if agent_state not in AGENT_STATE_NAMES or gate_state not in GATE_STATE_NAMES or kill_switch not in (0, 1):
            raise RuntimeError("invalid BotGuard status response enum")
        return StatusResult(
            AGENT_STATE_NAMES[agent_state],
            bool(kill_switch),
            GATE_STATE_NAMES[gate_state],
            durable_orders,
            active_reservations,
            total_reserved,
        )

    def _exchange(self, frame: bytes, expected_size: int) -> bytes:
        deadline = time.monotonic() + self._timeout
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
            connection.settimeout(self._remaining(deadline))
            connection.connect(self._socket_path)
            connection.settimeout(self._remaining(deadline))
            sent = connection.send(frame)
            if sent != len(frame):
                raise RuntimeError("short BotGuard request send")
            connection.settimeout(self._remaining(deadline))
            response = connection.recv(expected_size + 1)
        if len(response) != expected_size:
            raise RuntimeError("invalid BotGuard response size")
        return response

    @staticmethod
    def _require_zero_reserved(response: bytes, reserved: tuple[slice, ...]) -> None:
        if any(any(response[region]) for region in reserved):
            raise RuntimeError("non-zero BotGuard reserved response bytes")

    @staticmethod
    def _remaining(deadline: float) -> float:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("BotGuard request timed out")
        return remaining


def main() -> None:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    submit = commands.add_parser("submit")
    submit.add_argument("socket_path")
    submit.add_argument("strategy_id", type=int)
    submit.add_argument("client_order_id", type=int)
    submit.add_argument("market_id", type=int)
    submit.add_argument("side", choices=("buy", "sell"))
    submit.add_argument("price_micros", type=int)
    submit.add_argument("quantity_microunits", type=int)
    for command in ("status", "kill", "resume"):
        control = commands.add_parser(command)
        control.add_argument("socket_path")
    arguments = parser.parse_args()
    client = BotGuardClient(arguments.socket_path)
    if arguments.command == "submit":
        result = client.submit(
            strategy_id=arguments.strategy_id,
            client_order_id=arguments.client_order_id,
            market_id=arguments.market_id,
            side=arguments.side,
            price_micros=arguments.price_micros,
            quantity_microunits=arguments.quantity_microunits,
        )
    elif arguments.command == "status":
        result = client.get_status()
    elif arguments.command == "resume":
        result = client.resume()
    else:
        result = client.kill()
    print(dataclasses.asdict(result))


if __name__ == "__main__":
    main()
