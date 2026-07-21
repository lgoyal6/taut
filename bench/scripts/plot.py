#!/usr/bin/env python3
"""Aggregate the benchmark CSVs (bench/data/) into summary tables and, if matplotlib is
available, the PLAN §7 plots. Matplotlib is optional: without it, this still writes the
aggregated summary CSVs and prints text tables, so the raw CSVs remain the source of truth.

Inputs  (written by run_matrix.sh):
  latency.csv     rr + open-loop latency rows (per run, per loss)
  wire.csv        interface bytes-on-wire per run (from /proc/net/dev on the sender veth)
  throughput.csv  clean-link saturating goodput per run

Outputs (bench/data/):
  summary_rr.csv          median/min/max of p50,p99,p999 round-trip latency vs loss
  summary_overhead.csv    median bandwidth-overhead ratio (bytes-on-wire / goodput) vs loss
  summary_throughput.csv  median clean-link goodput (Mbit/s)
  *.png                   only if matplotlib is importable

Usage: bench/scripts/plot.py [--data DIR] [--msg-size 512]
"""
import argparse
import csv
import os
import statistics
import sys
from collections import defaultdict

# taut lines first (the subject), then the baselines. cls is "" for tcp/enet.
SERIES_ORDER = [("taut", "1"), ("taut", "2"), ("tcp", ""), ("enet", "")]
SERIES_LABEL = {
    ("taut", "1"): "taut class 1",
    ("taut", "2"): "taut class 2",
    ("tcp", ""): "kernel TCP",
    ("enet", ""): "ENet",
}


def load(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def fnum(row, key):
    try:
        return float(row[key])
    except (KeyError, ValueError):
        return None


def agg(values):
    """(median, min, max) of a list, or (None,)*3 if empty."""
    vs = [v for v in values if v is not None]
    if not vs:
        return None, None, None
    return statistics.median(vs), min(vs), max(vs)


def summarize_latency(rows, mode):
    """{(transport,cls): {loss: {metric: [per-run values]}}} for the given mode."""
    out = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for r in rows:
        if r.get("mode") != mode:
            continue
        key = (r["transport"], r.get("cls", ""))
        loss = fnum(r, "loss_pct")
        for m in ("p50_ms", "p99_ms", "p999_ms"):
            v = fnum(r, m)
            if v is not None:
                out[key][loss][m].append(v)
    return out


def write_summary_rr(lat_rows, data_dir):
    s = summarize_latency(lat_rows, "rr")
    path = os.path.join(data_dir, "summary_rr.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["transport", "cls", "loss_pct", "metric", "median_ms", "min_ms", "max_ms", "runs"])
        for key in SERIES_ORDER:
            if key not in s:
                continue
            for loss in sorted(s[key]):
                for m in ("p50_ms", "p99_ms", "p999_ms"):
                    vals = s[key][loss][m]
                    med, lo, hi = agg(vals)
                    if med is None:
                        continue
                    w.writerow([key[0], key[1], loss, m[:-3], f"{med:.3f}", f"{lo:.3f}", f"{hi:.3f}", len(vals)])
    return s, path


def write_summary_overhead(lat_rows, wire_rows, data_dir, msg_size):
    """Overhead = bytes-on-wire (sender veth tx) / goodput bytes (received * msg_size), from
    the open-loop (sustained-load) runs, where retransmit volume is meaningful."""
    # index open-loop received by key+run
    recv = {}
    for r in lat_rows:
        if r.get("mode") != "latency":
            continue
        k = (r["transport"], r.get("cls", ""), fnum(r, "loss_pct"), r.get("run"))
        recv[k] = fnum(r, "received")
    by = defaultdict(lambda: defaultdict(list))
    for w in wire_rows:
        if w.get("mode") != "latency":
            continue
        k = (w["transport"], w.get("cls", ""), fnum(w, "loss_pct"), w.get("run"))
        got = recv.get(k)
        tx = fnum(w, "wire_tx_bytes")
        if got and got > 0 and tx is not None:
            ratio = tx / (got * msg_size)
            by[(w["transport"], w.get("cls", ""))][fnum(w, "loss_pct")].append(ratio)
    path = os.path.join(data_dir, "summary_overhead.csv")
    with open(path, "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["transport", "cls", "loss_pct", "overhead_median", "overhead_min", "overhead_max", "runs"])
        for key in SERIES_ORDER:
            if key not in by:
                continue
            for loss in sorted(by[key]):
                med, lo, hi = agg(by[key][loss])
                wr.writerow([key[0], key[1], loss, f"{med:.3f}", f"{lo:.3f}", f"{hi:.3f}", len(by[key][loss])])
    return by, path


def write_summary_throughput(thr_rows, data_dir):
    by = defaultdict(list)
    for r in thr_rows:
        v = fnum(r, "goodput_mbps")
        if v is not None:
            by[(r["transport"], r.get("cls", ""))].append(v)
    path = os.path.join(data_dir, "summary_throughput.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["transport", "cls", "goodput_mbps_median", "min", "max", "runs"])
        for key in SERIES_ORDER:
            if key not in by:
                continue
            med, lo, hi = agg(by[key])
            w.writerow([key[0], key[1], f"{med:.2f}", f"{lo:.2f}", f"{hi:.2f}", len(by[key])])
    return by, path


def print_table_rr(s):
    print("\n== Request-reply round-trip latency vs loss (median across runs, ms) ==")
    for metric in ("p50_ms", "p99_ms", "p999_ms"):
        print(f"\n  [{metric[:-3]}]")
        losses = sorted({l for key in s for l in s[key]})
        hdr = "    loss% | " + " | ".join(f"{SERIES_LABEL[k]:>13}" for k in SERIES_ORDER if k in s)
        print(hdr)
        for loss in losses:
            cells = []
            for k in SERIES_ORDER:
                if k not in s:
                    continue
                med, _, _ = agg(s[k].get(loss, {}).get(metric, []))
                cells.append(f"{med:13.2f}" if med is not None else f"{'-':>13}")
            print(f"    {loss:5.0f} | " + " | ".join(cells))


def print_table_overhead(by):
    if not by:
        print("\n(no open-loop runs -> no overhead table; run with RUN_OPENLOOP=1)")
        return
    print("\n== Bandwidth overhead (bytes-on-wire / goodput bytes), sustained load ==")
    losses = sorted({l for key in by for l in by[key]})
    print("    loss% | " + " | ".join(f"{SERIES_LABEL[k]:>13}" for k in SERIES_ORDER if k in by))
    for loss in losses:
        cells = []
        for k in SERIES_ORDER:
            if k not in by:
                continue
            med, _, _ = agg(by[k].get(loss, []))
            cells.append(f"{med:13.2f}" if med is not None else f"{'-':>13}")
        print(f"    {loss:5.0f} | " + " | ".join(cells))


def print_table_throughput(by):
    print("\n== Clean-link (0% loss) saturating goodput (Mbit/s, median) ==")
    for k in SERIES_ORDER:
        if k not in by:
            continue
        med, lo, hi = agg(by[k])
        print(f"    {SERIES_LABEL[k]:>13}: {med:8.2f}  (min {lo:.2f}, max {hi:.2f})")


def make_plots(s_rr, by_over, by_thr, data_dir):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("\n[matplotlib not available -> summary CSVs + tables only; that is fine, the")
        print(" raw CSVs are the source of truth. Install matplotlib and re-run for PNGs.]")
        return

    colors = {("taut", "1"): "#1f77b4", ("taut", "2"): "#2ca02c",
              ("tcp", ""): "#d62728", ("enet", ""): "#ff7f0e"}

    # Headline: p50 / p99 / p999 latency vs loss.
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5), sharex=True)
    for ax, metric in zip(axes, ("p50_ms", "p99_ms", "p999_ms")):
        for k in SERIES_ORDER:
            if k not in s_rr:
                continue
            losses = sorted(s_rr[k])
            med = [agg(s_rr[k][l][metric])[0] for l in losses]
            lo = [agg(s_rr[k][l][metric])[1] for l in losses]
            hi = [agg(s_rr[k][l][metric])[2] for l in losses]
            pts = [(l, m, a, b) for l, m, a, b in zip(losses, med, lo, hi) if m is not None]
            if not pts:
                continue
            xs = [p[0] for p in pts]
            ms = [p[1] for p in pts]
            err = [[m - p[2] for m, p in zip(ms, pts)], [p[3] - m for m, p in zip(ms, pts)]]
            ax.errorbar(xs, ms, yerr=err, marker="o", capsize=3, label=SERIES_LABEL[k],
                        color=colors.get(k))
        ax.set_title(metric[:-3] + " round-trip latency")
        ax.set_xlabel("loss (%, each direction)")
        ax.set_ylabel("latency (ms)")
        ax.grid(True, alpha=0.3)
        ax.legend()
    fig.suptitle("taut vs kernel-TCP vs ENet: request-reply latency under loss (RTT 30 ms)")
    fig.tight_layout()
    fig.savefig(os.path.join(data_dir, "latency_vs_loss.png"), dpi=110)
    plt.close(fig)

    # We-lose-here #1: bandwidth overhead vs loss.
    if by_over:
        fig, ax = plt.subplots(figsize=(6.5, 4.5))
        for k in SERIES_ORDER:
            if k not in by_over:
                continue
            losses = sorted(by_over[k])
            med = [agg(by_over[k][l])[0] for l in losses]
            ax.plot(losses, med, marker="s", label=SERIES_LABEL[k], color=colors.get(k))
        ax.axhline(1.0, color="gray", ls="--", alpha=0.6, label="ideal (1.0)")
        ax.set_title("Bandwidth overhead vs loss (sustained load) — taut pays for its tail")
        ax.set_xlabel("loss (%, each direction)")
        ax.set_ylabel("bytes on wire / goodput bytes")
        ax.grid(True, alpha=0.3)
        ax.legend()
        fig.tight_layout()
        fig.savefig(os.path.join(data_dir, "overhead_vs_loss.png"), dpi=110)
        plt.close(fig)

    # We-lose-here #2: clean-link throughput bar.
    if by_thr:
        fig, ax = plt.subplots(figsize=(6.5, 4.5))
        keys = [k for k in SERIES_ORDER if k in by_thr]
        vals = [agg(by_thr[k])[0] for k in keys]
        ax.bar([SERIES_LABEL[k] for k in keys], vals,
               color=[colors.get(k) for k in keys])
        ax.set_title("Clean-link (0% loss) throughput — TCP/ENet win")
        ax.set_ylabel("goodput (Mbit/s)")
        ax.grid(True, axis="y", alpha=0.3)
        for i, v in enumerate(vals):
            ax.text(i, v, f"{v:.1f}", ha="center", va="bottom")
        fig.tight_layout()
        fig.savefig(os.path.join(data_dir, "throughput_cleanlink.png"), dpi=110)
        plt.close(fig)

    print(f"\n[plots written to {data_dir}/*.png]")


def main():
    ap = argparse.ArgumentParser()
    default_data = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
    ap.add_argument("--data", default=os.path.normpath(default_data))
    ap.add_argument("--msg-size", type=int, default=512)
    args = ap.parse_args()

    lat = load(os.path.join(args.data, "latency.csv"))
    wire = load(os.path.join(args.data, "wire.csv"))
    thr = load(os.path.join(args.data, "throughput.csv"))
    if not lat and not thr:
        print(f"no CSVs found in {args.data}; run bench/scripts/run_matrix.sh first", file=sys.stderr)
        return 1

    s_rr, p1 = write_summary_rr(lat, args.data)
    by_over, p2 = write_summary_overhead(lat, wire, args.data, args.msg_size)
    by_thr, p3 = write_summary_throughput(thr, args.data)
    print(f"wrote {p1}\nwrote {p2}\nwrote {p3}")

    print_table_rr(s_rr)
    print_table_overhead(by_over)
    print_table_throughput(by_thr)
    make_plots(s_rr, by_over, by_thr, args.data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
