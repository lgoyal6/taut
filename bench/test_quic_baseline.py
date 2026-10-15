"""Checks QUIC framing against the native benchmark payload contract.
Covers timestamp/sequence layout, filler bytes and the end sentinel.
These byte-level tests do not replace the real TLS integration study.
"""
import struct
import unittest

from quic_baseline import END_SEQ, MESSAGE_SIZE, frame


class QuicFrameTest(unittest.TestCase):
    def test_payload_matches_tcp_and_taut_benchmark_layout(self):
        encoded = frame(123456789, 42)
        self.assertEqual(struct.unpack_from("<I", encoded)[0], MESSAGE_SIZE)
        payload = encoded[4:]
        self.assertEqual(len(payload), MESSAGE_SIZE)
        self.assertEqual(struct.unpack_from("<QQ", payload), (123456789, 42))
        self.assertEqual(payload[16], (16 * 31 + 7) & 0xFF)
        self.assertEqual(payload[-1], (511 * 31 + 7) & 0xFF)

    def test_end_sequence_uses_the_existing_benchmark_sentinel(self):
        self.assertEqual(struct.unpack_from("<Q", frame(1, END_SEQ), 4 + 8)[0], END_SEQ)


if __name__ == "__main__":
    unittest.main()
