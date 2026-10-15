#!/usr/bin/env python3
"""Counts repeated STREAM bytes from the pinned aioquic qlog event format.
Uses stream offsets to distinguish new bytes from retransmitted overlapping ranges.
Excludes QUIC packet overhead and crypto/control frames; wire totals are separate.
"""
import argparse
import hashlib
import json
from pathlib import Path


def stream_bytes(document):
    transmitted = unique = frames = 0
    for trace in document['traces']:
        ranges = {}
        for event in trace['events']:
            if event['name'] != 'transport:packet_sent':
                continue
            for frame in event['data'].get('frames', []):
                if frame.get('frame_type') != 'stream':
                    continue
                stream, offset, length = frame['stream_id'], frame['offset'], frame['length']
                if not all(isinstance(x, int) and x >= 0 for x in (stream, offset, length)):
                    raise ValueError('invalid STREAM range in qlog')
                frames += 1
                transmitted += length
                if length:
                    ranges.setdefault(stream, []).append((offset, offset + length))
        for spans in ranges.values():
            end = 0
            for left, right in sorted(spans):
                unique += max(0, right - max(left, end))
                end = max(end, right)
    return dict(stream_frames=frames, transmitted_stream_bytes=transmitted,
                unique_stream_bytes=unique, retransmitted_stream_bytes=transmitted-unique,
                scope='sent STREAM payload ranges, including application framing; excludes crypto, control and packet headers')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('qlog', type=Path, nargs='+')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    rows = [dict(path=str(p), sha256=hashlib.sha256(p.read_bytes()).hexdigest(), **stream_bytes(json.loads(p.read_text()))) for p in args.qlog]
    args.out.write_text(json.dumps(rows, indent=2)+'\n')
    print(json.dumps(rows, indent=2))

if __name__ == '__main__':
    main()
