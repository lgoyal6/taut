"""Checks paced-client accounting at the completion cutoff.
Local stream fixtures isolate timely and delayed reply cases.
Real aioquic/TLS operation is verified separately by run_paced_study.py.
"""
import argparse
import asyncio
import csv
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch
import quic_baseline as bench

class Configuration:
    def __init__(self, **kwargs): pass
    def load_verify_locations(self, **kwargs): pass

class Connection:
    def __init__(self, delay):
        self.delay=delay
        self.reader=asyncio.StreamReader()
    async def __aenter__(self): return self
    async def __aexit__(self,*args): pass
    async def create_stream(self): return self.reader,self
    def write(self, b):
        asyncio.get_running_loop().call_later(self.delay,self.reader.feed_data,b)
    async def drain(self): pass
    def close(self): pass

class PacedAccountingTest(unittest.IsolatedAsyncioTestCase):
    async def check_delay(self,delay):
        with tempfile.TemporaryDirectory() as temp:
            args=argparse.Namespace(rate=1,duration=1,drain=0,idle_timeout=5,ca='fixture',
                                    host='fixture',port=1,start_ns=time.monotonic_ns()+10_000_000,
                                    out=str(Path(temp)/'row.csv'),mode='paced')
            with patch.object(bench,'QuicConfiguration',Configuration), patch.object(bench,'connect',lambda *a,**k:Connection(delay)):
                status=await bench.run_paced_client(args)
            with Path(args.out).open() as output:
                row=next(csv.DictReader(output))
            self.assertEqual(int(row['offered']),int(row['accepted'])+int(row['rejected']))
            self.assertEqual(int(row['accepted']),int(row['completed'])+int(row['missing']))
            return row,status
    async def test_peer_eof_releases_the_single_stream_server(self):
        reader = asyncio.StreamReader()
        reader.feed_eof()
        class Writer:
            closed = False
            def close(self): self.closed = True
        writer, done = Writer(), asyncio.Event()
        await bench.echo_stream(reader, writer, done)
        await asyncio.wait_for(done.wait(), timeout=1)
        self.assertTrue(writer.closed)

    async def test_timely_echo_completes(self):
        row,status=await self.check_delay(0)
        self.assertEqual(status,0)
        self.assertEqual(int(row['completed']),1)
    async def test_reply_after_drain_cap_remains_missing(self):
        row,status=await self.check_delay(1.1)
        self.assertEqual(status,1)
        self.assertEqual(int(row['completed']),0)
        self.assertEqual(int(row['missing']),1)

if __name__=='__main__': unittest.main()
