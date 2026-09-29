import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "clients" / "python"))

import botguard_client as client_module


class FakeSeqpacketSocket:
    def __init__(self, response: bytes) -> None:
        self.response = response
        self.timeouts: list[float] = []
        self.sent: list[bytes] = []

    def __enter__(self):
        return self

    def __exit__(self, *_args) -> None:
        return None

    def settimeout(self, timeout: float) -> None:
        self.timeouts.append(timeout)

    def connect(self, _path: str) -> None:
        return None

    def send(self, frame: bytes) -> int:
        self.sent.append(frame)
        return len(frame)

    def recv(self, _size: int) -> bytes:
        return self.response


class BotGuardClientTest(unittest.TestCase):
    def test_protocol_v1_reason_bit_16_uses_baseline_semantics(self) -> None:
        self.assertEqual(client_module.REJECT_REASON_NAMES[16], "MAX_LOSS_SINCE_BASELINE_EXCEEDED")

    def test_uses_one_send_and_one_decreasing_end_to_end_deadline(self) -> None:
        response = client_module.STATUS_RESPONSE_FRAME.pack(
            client_module.MAGIC,
            client_module.VERSION,
            client_module.STATUS_RESPONSE,
            1,
            0,
            1,
            0,
            0,
            0,
        )
        connection = FakeSeqpacketSocket(response)
        with mock.patch.object(client_module.socket, "socket", return_value=connection), mock.patch.object(
            client_module.time, "monotonic", side_effect=(100.0, 101.0, 102.0, 103.0)
        ):
            result = client_module.BotGuardClient("/agent.sock", timeout=5.0).get_status()

        self.assertEqual(result.agent_state, "READY")
        self.assertEqual(len(connection.sent), 1)
        self.assertEqual(connection.timeouts, [4.0, 3.0, 2.0])

    def test_rejects_non_zero_reserved_bytes_for_every_response_type(self) -> None:
        submit = bytearray(client_module.RESPONSE.size)
        submit[12] = 1
        status = bytearray(client_module.STATUS_RESPONSE_FRAME.size)
        status[11] = 1
        resume_first = bytearray(client_module.RESUME_RESPONSE_FRAME.size)
        resume_first[9] = 1
        resume_second = bytearray(client_module.RESUME_RESPONSE_FRAME.size)
        resume_second[19] = 1

        with self.assertRaisesRegex(RuntimeError, "reserved"):
            client_module.BotGuardClient._require_zero_reserved(bytes(submit), (slice(12, 16),))
        with self.assertRaisesRegex(RuntimeError, "reserved"):
            client_module.BotGuardClient._require_zero_reserved(bytes(status), (slice(11, 16),))
        for response in (resume_first, resume_second):
            with self.assertRaisesRegex(RuntimeError, "reserved"):
                client_module.BotGuardClient._require_zero_reserved(
                    bytes(response), (slice(9, 16), slice(19, 24))
                )

    def test_rejects_invalid_timeout(self) -> None:
        for timeout in (0.0, -1.0, float("inf"), float("nan")):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                client_module.BotGuardClient("/agent.sock", timeout=timeout)


if __name__ == "__main__":
    unittest.main()
