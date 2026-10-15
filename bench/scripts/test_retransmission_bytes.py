"""Checks qlog range accounting used by retransmission_bytes.py.
Synthetic ranges expose duplicate, overlapping and per-connection stream cases.
Real aioquic output is validated separately in the local accounting supplement.
"""
import unittest
from retransmission_bytes import stream_bytes


def trace(*ranges):
    return dict(events=[dict(name='transport:packet_sent', data=dict(frames=[dict(frame_type='stream',stream_id=stream,offset=offset,length=length)])) for stream,offset,length in ranges])


class StreamByteControls(unittest.TestCase):
    def test_new_bytes_and_partial_overlap(self):
        result=stream_bytes(dict(traces=[trace((0,0,100),(0,50,100),(0,80,10))]))
        self.assertEqual(result['transmitted_stream_bytes'],210)
        self.assertEqual(result['unique_stream_bytes'],150)
        self.assertEqual(result['retransmitted_stream_bytes'],60)
    def test_repeated_frame_is_retransmission(self):
        self.assertEqual(stream_bytes(dict(traces=[trace((0,0,100),(0,0,100))]))['retransmitted_stream_bytes'],100)
    def test_stream_and_connection_offsets_do_not_alias(self):
        result=stream_bytes(dict(traces=[trace((0,0,100),(4,0,100)),trace((0,0,100))]))
        self.assertEqual(result['retransmitted_stream_bytes'],0)
    def test_invalid_offset_is_rejected(self):
        with self.assertRaises(ValueError):stream_bytes(dict(traces=[trace((0,-1,10))]))

if __name__=='__main__':unittest.main()
