#!/usr/bin/env python3
"""End-to-end validation of TickForge against LOBSTER ground truth.

Runs two checks on a LOBSTER level-N sample:

1. **Seed-state correctness** -- replay only the synthetic seed ADD events
   emitted by `lobster_to_tickforge.py --initial-book ...` and confirm
   TickForge's final book matches LOBSTER's published orderbook[0] exactly.
   This validates the OrderBook engine and adapter logic.

2. **End-of-session reconstruction** -- replay the full converted event
   stream and report the diff against LOBSTER's published last orderbook
   row. Mismatches at end-of-session are expected on level-N data because
   LOBSTER's level-N message file only contains events that affect the
   top-N levels at the time of the event; orders that fall out of the
   top-N due to later activity have their subsequent cancels filtered
   out, so a perfect end-of-session match is not achievable without
   full-depth data.

Usage:
    python3 scripts/validate_lobster_replay.py \\
        --replay-bin ./build/tickforge_replay \\
        --events     data/aapl_2012-06-21.csv \\
        --orderbook  data/lobster_aapl_2012-06-21/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv
"""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from pathlib import Path


LOBSTER_SENTINEL = 9_999_999_999


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--replay-bin", type=Path, required=True)
    p.add_argument("--events",     type=Path, required=True)
    p.add_argument("--orderbook",  type=Path, required=True)
    p.add_argument("--levels",     type=int, default=10)
    return p.parse_args()


def read_lobster_row(path: Path, row_index: int, levels: int) -> tuple[list[tuple[int,int]], list[tuple[int,int]]]:
    with path.open() as f:
        for i, line in enumerate(f):
            if i == row_index:
                row = line.strip()
                break
        else:
            raise SystemExit(f"orderbook only has {i+1} rows, requested {row_index}")
    fields = [int(x) for x in row.split(",")]
    asks: list[tuple[int,int]] = []
    bids: list[tuple[int,int]] = []
    for i in range(levels):
        a_p, a_s, b_p, b_s = fields[4*i:4*i+4]
        if abs(a_p) != LOBSTER_SENTINEL and a_s > 0:
            asks.append((a_p, a_s))
        if abs(b_p) != LOBSTER_SENTINEL and b_s > 0:
            bids.append((b_p, b_s))
    return bids, asks


def replay(replay_bin: Path, events_path: Path) -> tuple[list[tuple[int,int]], list[tuple[int,int]], dict[str,str]]:
    r = subprocess.run(
        [str(replay_bin), str(events_path), "--mode", "max", "--allow-orphan-events"],
        capture_output=True, text=True, check=True,
    )
    bids: list[tuple[int,int]] = []
    asks: list[tuple[int,int]] = []
    summary: dict[str,str] = {}
    for line in r.stdout.splitlines():
        if line.startswith("top_") and "_bids:" in line:
            bids = [_parse(p) for p in line.split(":",1)[1].split() if p]
        elif line.startswith("top_") and "_asks:" in line:
            asks = [_parse(p) for p in line.split(":",1)[1].split() if p]
        elif ":" in line:
            k, _, v = line.partition(":")
            summary[k.strip()] = v.strip()
    return bids, asks, summary


def _parse(token: str) -> tuple[int,int]:
    p, _, s = token.partition("@")
    return int(p), int(s)


def diff(tf: list[tuple[int,int]], lob: list[tuple[int,int]], levels: int) -> int:
    n = 0
    for i in range(levels):
        a = tf[i] if i < len(tf) else None
        b = lob[i] if i < len(lob) else None
        if a != b:
            n += 1
    return n


def count_seed_events(events_path: Path) -> int:
    n = 0
    with events_path.open() as f:
        reader = csv.reader(f)
        next(reader, None)  # header
        for row in reader:
            if len(row) < 6:
                continue
            if int(row[0]) == 0:  # seed events all have timestamp 0
                n += 1
            else:
                break
    return n


def main() -> int:
    args = parse_args()

    print("=" * 72)
    print("TickForge vs LOBSTER -- Correctness Validation")
    print("=" * 72)

    # --- Check 1: seed-only state vs orderbook[0] -----------------------
    n_seeds = count_seed_events(args.events)
    if n_seeds == 0:
        print("\nNo seed events found in input -- run lobster_to_tickforge.py with --initial-book first.")
        return 1

    seed_only = args.events.parent / (args.events.stem + "_seed_only.csv")
    with args.events.open() as src, seed_only.open("w") as dst:
        dst.write(next(src))  # header
        for _ in range(n_seeds):
            dst.write(next(src))

    tf_b, tf_a, sum_seed = replay(args.replay_bin, seed_only)
    lob_b, lob_a = read_lobster_row(args.orderbook, 0, args.levels)
    seed_only.unlink()

    mb = diff(tf_b, lob_b, args.levels)
    ma = diff(tf_a, lob_a, args.levels)

    print(f"\nCheck 1: seed-only replay vs LOBSTER orderbook[0]")
    print(f"  events_processed: {sum_seed.get('events_processed')}")
    print(f"  bid mismatches: {mb}/{args.levels}")
    print(f"  ask mismatches: {ma}/{args.levels}")
    seed_ok = (mb == 0 and ma == 0)
    print(f"  result: {'PASS' if seed_ok else 'FAIL'} -- engine and adapter are {'correct' if seed_ok else 'BROKEN'}")

    # --- Check 2: full-day replay vs LOBSTER last orderbook row --------
    tf_b, tf_a, sum_full = replay(args.replay_bin, args.events)
    # count actual data rows in orderbook
    with args.orderbook.open() as f:
        last_row_index = sum(1 for _ in f) - 1
    lob_b, lob_a = read_lobster_row(args.orderbook, last_row_index, args.levels)

    mb = diff(tf_b, lob_b, args.levels)
    ma = diff(tf_a, lob_a, args.levels)

    print(f"\nCheck 2: full-day replay vs LOBSTER orderbook[end] (row {last_row_index})")
    print(f"  events_processed: {sum_full.get('events_processed')}")
    print(f"  rejected_events:  {sum_full.get('rejected_events')}")
    print(f"  throughput:       {sum_full.get('throughput_events_per_sec')} events/sec")
    print(f"  latency p50/p95/p99: {sum_full.get('latency_p50_ns')} / "
          f"{sum_full.get('latency_p95_ns')} / {sum_full.get('latency_p99_ns')} ns")
    print(f"  bid mismatches: {mb}/{args.levels}")
    print(f"  ask mismatches: {ma}/{args.levels}")
    print(f"  note: end-of-session divergence on LEVEL-N data is EXPECTED.")
    print(f"        LOBSTER's level-N message file omits events that fall")
    print(f"        outside the top-N range, so a perfect reconstruction is")
    print(f"        not achievable without full-depth data. See:")
    print(f"        https://data.lobsterdata.com/info/HowDoesItWork.php")

    print("\n" + "=" * 72)
    return 0 if seed_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
