#!/usr/bin/env python3
"""Compare TickForge's final top-N book against LOBSTER's published snapshot.

LOBSTER's `orderbook` file holds the post-event state for every message row.
The last row is the end-of-session published book. This script reads that row,
runs `tickforge_replay --mode max` on the converted message CSV, parses the
`top_10_bids:` / `top_10_asks:` lines from TickForge's stdout, and prints a
level-by-level diff.

Usage:
    python3 scripts/compare_lobster_snapshot.py \\
        --replay-bin ./build/tickforge_replay \\
        --events    data/aapl_2012-06-21.csv \\
        --orderbook data/lobster_aapl_2012-06-21/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--replay-bin", type=Path, required=True)
    p.add_argument("--events", type=Path, required=True, help="TickForge-format CSV")
    p.add_argument("--orderbook", type=Path, required=True, help="LOBSTER orderbook CSV")
    p.add_argument("--levels", type=int, default=10)
    return p.parse_args()


def read_lobster_last_row(path: Path, levels: int) -> tuple[list[tuple[int, int]], list[tuple[int, int]]]:
    with path.open() as f:
        last = None
        for line in f:
            line = line.strip()
            if line:
                last = line
    if last is None:
        raise SystemExit(f"empty orderbook file: {path}")

    fields = [int(x) for x in last.split(",")]
    if len(fields) < 4 * levels:
        raise SystemExit(f"orderbook row has only {len(fields)} columns, expected >= {4 * levels}")

    asks: list[tuple[int, int]] = []
    bids: list[tuple[int, int]] = []
    for i in range(levels):
        a_p = fields[4 * i + 0]
        a_s = fields[4 * i + 1]
        b_p = fields[4 * i + 2]
        b_s = fields[4 * i + 3]
        # LOBSTER fills empty levels with sentinel +/- 9_999_999_999.
        if abs(a_p) != 9_999_999_999:
            asks.append((a_p, a_s))
        if abs(b_p) != 9_999_999_999:
            bids.append((b_p, b_s))
    return bids, asks


def run_tickforge(replay_bin: Path, events: Path) -> tuple[list[tuple[int, int]], list[tuple[int, int]], dict[str, str]]:
    result = subprocess.run(
        [str(replay_bin), str(events), "--mode", "max"],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit(f"tickforge_replay exited with {result.returncode}")

    bids: list[tuple[int, int]] = []
    asks: list[tuple[int, int]] = []
    summary: dict[str, str] = {}

    for line in result.stdout.splitlines():
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        key = key.strip()
        value = value.strip()
        if key.startswith("top_") and key.endswith("_bids"):
            bids = [_parse_level(p) for p in value.split() if p]
        elif key.startswith("top_") and key.endswith("_asks"):
            asks = [_parse_level(p) for p in value.split() if p]
        else:
            summary[key] = value
    return bids, asks, summary


def _parse_level(token: str) -> tuple[int, int]:
    price, _, size = token.partition("@")
    return int(price), int(size)


def fmt_level(level: tuple[int, int] | None) -> str:
    if level is None:
        return f"{'—':>14}"
    p, s = level
    return f"{p:>9}@{s:<6}"


def diff_side(label: str, tf: list[tuple[int, int]], lob: list[tuple[int, int]], levels: int) -> int:
    print(f"\n{label} side (level: TickForge vs LOBSTER published)")
    print("-" * 68)
    mismatches = 0
    for i in range(levels):
        a = tf[i] if i < len(tf) else None
        b = lob[i] if i < len(lob) else None
        marker = "  " if a == b else "!="
        if a != b:
            mismatches += 1
        print(f" L{i+1:>2}  {fmt_level(a)}   {marker}   {fmt_level(b)}")
    return mismatches


def main() -> int:
    args = parse_args()

    lob_bids, lob_asks = read_lobster_last_row(args.orderbook, args.levels)
    tf_bids, tf_asks, summary = run_tickforge(args.replay_bin, args.events)

    print("=" * 68)
    print("TickForge vs LOBSTER published snapshot")
    print("=" * 68)
    print(f" events_processed : {summary.get('events_processed', '?')}")
    print(f" rejected_events  : {summary.get('rejected_events', '?')}")
    print(f" final_best_bid   : {summary.get('final_best_bid', '?')}")
    print(f" final_best_ask   : {summary.get('final_best_ask', '?')}")
    print(f" final_spread     : {summary.get('final_spread', '?')}")

    bid_mismatches = diff_side("BID", tf_bids, lob_bids, args.levels)
    ask_mismatches = diff_side("ASK", tf_asks, lob_asks, args.levels)

    print("\n" + "=" * 68)
    print(f"Bid mismatches: {bid_mismatches}/{args.levels}")
    print(f"Ask mismatches: {ask_mismatches}/{args.levels}")
    print("=" * 68)

    return 0 if (bid_mismatches == 0 and ask_mismatches == 0) else 1


if __name__ == "__main__":
    raise SystemExit(main())
