#!/usr/bin/env python3
"""Runs the real pinned aioquic arm with verified local TLS.
Uses the same payload and admission contract as paced_bench.cc.
Raw timing files feed analyze_paced_study.py; RR remains a closed-loop control.
"""
import argparse
import asyncio
import csv
from contextlib import suppress
import math
import json
import resource
import ssl
import sys
import struct
import time
from pathlib import Path

import aioquic
from aioquic.asyncio import QuicConnectionProtocol, connect, serve
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.logger import QuicLogger

MESSAGE_SIZE = 512
HEADER_SIZE = 16
END_SEQ = (1 << 64) - 1
ALPN = ["b5-rr/1"]


def frame(ts_ns: int, seq: int) -> bytes:
    payload = bytearray(MESSAGE_SIZE)
    struct.pack_into("<QQ", payload, 0, ts_ns, seq)
    for i in range(HEADER_SIZE, MESSAGE_SIZE):
        payload[i] = (i * 31 + 7) & 0xFF
    return struct.pack("<I", MESSAGE_SIZE) + payload


def percentiles(samples):
    if not samples:
        return [0.0] * 5
    xs = sorted(samples)
    def rank(p): return xs[max(0, min(len(xs) - 1, math.ceil(p * len(xs)) - 1))]
    return [rank(0.50), rank(0.90), rank(0.99), rank(0.999), xs[-1]]


async def echo_stream(reader, writer, done):
    try:
        while True:
            hdr = await reader.readexactly(4)
            n = struct.unpack("<I", hdr)[0]
            if n != MESSAGE_SIZE:
                return
            body = await reader.readexactly(n)
            seq = struct.unpack_from("<Q", body, 8)[0]
            writer.write(hdr + body)
            await writer.drain()
            if seq == END_SEQ:
                asyncio.get_running_loop().call_later(0.2, done.set)
                return
    except asyncio.IncompleteReadError:
        return
    finally:
        # The only study stream owns this server. Also finish after peer EOF or
        # invalid framing, so an abandoned connection cannot occupy a later port.
        asyncio.get_running_loop().call_later(0.2, done.set)
        writer.close()


async def run_server(args):
    config = QuicConfiguration(is_client=False, alpn_protocols=ALPN, idle_timeout=args.idle_timeout)
    logger = QuicLogger() if getattr(args, "qlog", None) else None
    config.quic_logger = logger
    config.load_cert_chain(args.cert, args.key)
    done = asyncio.Event()
    server = await serve(args.host, args.port, configuration=config, stream_handler=lambda reader, writer: asyncio.create_task(echo_stream(reader, writer, done)))
    print(f"server-ready aioquic={aioquic.__version__} listen={args.host}:{args.port}", flush=True)
    try:
        await asyncio.wait_for(done.wait(), timeout=args.idle_timeout)
    finally:
        server.close()
        if logger:
            Path(args.qlog).write_text(json.dumps(logger.to_dict()))
        print(f"server cpu_s={time.process_time():.6f}", flush=True)


async def run_client(args):
    config = QuicConfiguration(is_client=True, alpn_protocols=ALPN, idle_timeout=args.idle_timeout)
    config.verify_mode = ssl.CERT_REQUIRED
    config.server_name = "localhost"
    config.load_verify_locations(cafile=args.ca)
    samples = []
    sent = replies = errors = 0
    started_cpu = time.process_time()
    started = time.monotonic_ns()
    deadline = started + args.duration * 1_000_000_000
    async with connect(args.host, args.port, configuration=config,
                       create_protocol=QuicConnectionProtocol) as protocol:
        reader, writer = await protocol.create_stream()
        while time.monotonic_ns() < deadline:
            payload = frame(time.monotonic_ns(), sent)
            writer.write(payload)
            await writer.drain()
            sent += 1
            try:
                hdr = await asyncio.wait_for(reader.readexactly(4), timeout=args.response_timeout)
                n = struct.unpack("<I", hdr)[0]
                if n != MESSAGE_SIZE:
                    errors += 1
                    break
                body = await asyncio.wait_for(reader.readexactly(n), timeout=args.response_timeout)
                ts_ns, seq = struct.unpack_from("<QQ", body)
                if seq != sent - 1:
                    errors += 1
                else:
                    replies += 1
                    samples.append((time.monotonic_ns() - ts_ns) / 1e6)
            except (asyncio.TimeoutError, asyncio.IncompleteReadError):
                errors += 1
                break
        writer.write(frame(time.monotonic_ns(), END_SEQ))
        await writer.drain()
        try:
            hdr = await asyncio.wait_for(reader.readexactly(4), timeout=args.response_timeout)
            n = struct.unpack("<I", hdr)[0]
            await asyncio.wait_for(reader.readexactly(n), timeout=args.response_timeout)
        except (asyncio.TimeoutError, asyncio.IncompleteReadError):
            errors += 1
        writer.close()
    cpu_s = time.process_time() - started_cpu
    rss_kib = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if sys.platform == "darwin":
        rss_kib //= 1024
    p50, p90, p99, p999, maximum = percentiles(samples)
    row = {
        "transport": "quic", "cls": "", "mode": "rr", "loss_pct": args.loss,
        "rtt_ms": args.rtt, "rate": args.rate, "run": args.run, "seed": args.seed,
        "sent": sent, "replies": replies, "p50_ms": p50, "p90_ms": p90,
        "p99_ms": p99, "p999_ms": p999, "min_ms": min(samples, default=0),
        "max_ms": maximum, "mean_ms": sum(samples) / len(samples) if samples else 0,
        "errors": errors, "cpu_s": cpu_s, "client_maxrss_kib": rss_kib,
        "implementation": f"aioquic=={aioquic.__version__}", "payload_bytes": MESSAGE_SIZE,
        "requests_in_flight": 1,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(row))
        if f.tell() == 0:
            writer.writeheader()
        writer.writerow(row)
    print("QUIC_RR", " ".join(f"{k}={v}" for k, v in row.items()), flush=True)
    return 0 if errors == 0 and replies == sent else 1



async def run_paced_client(args):
    """Same bounded fixed-cadence offers and full echo oracle as paced_bench.cc."""
    if not (0 < args.rate <= 10000 and 0 < args.duration <= 30 and 0 <= args.drain <= 30):
        raise ValueError("rate/duration/drain outside bounded protocol")
    count = int(args.rate * args.duration)
    if count > 100000:
        raise ValueError("study offer limit exceeded")
    interval = int(1e9 / args.rate)
    config = QuicConfiguration(is_client=True, alpn_protocols=ALPN, idle_timeout=args.idle_timeout)
    logger = QuicLogger() if getattr(args, "qlog", None) else None
    config.quic_logger = logger
    config.verify_mode = ssl.CERT_REQUIRED
    config.server_name = "localhost"
    config.load_verify_locations(cafile=args.ca)
    offers_file = Path(args.out + ".offers.csv").open("w", newline="")
    samples_file = Path(args.out + ".samples.csv").open("w", newline="")
    offers_writer, samples_writer = csv.writer(offers_file), csv.writer(samples_file)
    offers_writer.writerow(["seq", "intended_ns", "observed_ns", "accepted"])
    samples_writer.writerow(["seq", "intended_ns", "completed_ns"])
    pending, samples = {}, []
    completions_frozen = False
    offered = accepted = rejected = completed = in_window = errors = late = peak = 0
    cpu_start = time.process_time()
    async with connect(args.host, args.port, configuration=config) as protocol:
        reader, writer = await protocol.create_stream()
        start = args.start_ns or time.monotonic_ns() + 200_000_000
        end = start + args.duration * 1_000_000_000
        cap = end + args.drain * 1_000_000_000
        async def replies():
            nonlocal completed, in_window, errors
            try:
                while True:
                    hdr = await reader.readexactly(4)
                    if struct.unpack("<I", hdr)[0] != MESSAGE_SIZE:
                        errors += 1
                        return
                    body = await reader.readexactly(MESSAGE_SIZE)
                    ts, seq = struct.unpack_from("<QQ", body)
                    if seq == END_SEQ:
                        return
                    if pending.get(seq) != ts or body != frame(ts, seq)[4:]:
                        errors += 1
                        continue
                    now = time.monotonic_ns()
                    if completions_frozen or now >= cap:
                        continue # retain as missing at the frozen boundary
                    samples_writer.writerow([seq, ts, now])
                    del pending[seq]
                    completed += 1
                    in_window += now < end
                    samples.append((now - ts) / 1e6)
            except asyncio.IncompleteReadError:
                if pending:
                    errors += 1
        task = asyncio.create_task(replies())
        while time.monotonic_ns() < cap:
            if args.mode == "bulk" and offered >= 100000:
                errors += 1
                break
            now = time.monotonic_ns()
            while (args.mode != "bulk" and offered < count and start + offered * interval <= now) or (args.mode == "bulk" and start <= now < end and len(pending) < 64):
                seq = offered
                intended = now if args.mode == "bulk" else start + seq * interval
                offered += 1
                late += now > intended + 1_000_000
                accept = now < end and now <= intended + interval and len(pending) < 64
                offers_writer.writerow([seq, intended, now, int(accept)])
                if not accept:
                    rejected += 1
                else:
                    pending[seq] = intended
                    writer.write(frame(intended, seq))
                    accepted += 1
                    peak = max(peak, len(pending))
                now = time.monotonic_ns()
            if (args.mode == "bulk" or offered == count) and not pending and now >= end:
                break
            await asyncio.sleep(0.0001)
        # Freeze the completion boundary before teardown. A late echo must not both
        # remain counted as missing and be credited as completed after the drain cap.
        completions_frozen = True
        missing = len(pending)
        errors += missing
        writer.write(frame(time.monotonic_ns(), END_SEQ))
        await writer.drain()
        # Read the end echo before closing the reliable stream. Completion
        # counters remain frozen, including when ordinary replies arrive late.
        try:
            await asyncio.wait_for(task, timeout=2)
        except asyncio.TimeoutError:
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task
        writer.close()
    offers_file.close()
    samples_file.close()
    if logger:
        Path(args.qlog).write_text(json.dumps(logger.to_dict()))
    p50, _, p99, _, maximum = percentiles(samples)
    row = dict(transport="quic", offered=offered, accepted=accepted, rejected=rejected,
               completed=completed, completed_in_window=in_window, missing=missing,
               errors=errors, late_offers=late, peak_inflight=peak,
               p50_ms=p50, p99_ms=p99, max_ms=maximum,
               cpu_s=time.process_time() - cpu_start, retransmits="",
               goodput_mbps=in_window * MESSAGE_SIZE * 8 / args.duration / 1e6)
    with Path(args.out).open("a", newline="") as f:
        output = csv.DictWriter(f, fieldnames=row)
        if f.tell() == 0:
            output.writeheader()
        output.writerow(row)
    print("QUIC_PACED", row, flush=True)
    return int(bool(errors or offered != accepted + rejected or accepted != completed))


def parser():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="role", required=True)
    s = sub.add_parser("server")
    s.add_argument("--host", required=True)
    s.add_argument("--port", type=int, default=9101)
    s.add_argument("--cert", required=True)
    s.add_argument("--key", required=True)
    s.add_argument("--qlog")
    s.add_argument("--idle-timeout", type=float, default=45)
    c = sub.add_parser("client")
    c.add_argument("--host", required=True)
    c.add_argument("--port", type=int, default=9101)
    c.add_argument("--ca", required=True)
    c.add_argument("--duration", type=int, default=3)
    c.add_argument("--response-timeout", type=float, default=20)
    c.add_argument("--idle-timeout", type=float, default=45)
    c.add_argument("--loss", type=float, required=True)
    c.add_argument("--rtt", type=float, required=True)
    c.add_argument("--rate", type=float, default=0)
    c.add_argument("--run", type=int, required=True)
    c.add_argument("--seed", type=int, required=True)
    c.add_argument("--out", required=True)
    c.add_argument("--qlog")
    c.add_argument("--mode", choices=["rr", "paced", "bulk"], default="rr")
    c.add_argument("--start-ns", type=int, default=0)
    c.add_argument("--drain", type=int, default=15)
    return p


async def main():
    args = parser().parse_args()
    if args.role == "server":
        await run_server(args)
        return 0
    return await (run_paced_client(args) if args.mode in ("paced", "bulk") else run_client(args))


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
