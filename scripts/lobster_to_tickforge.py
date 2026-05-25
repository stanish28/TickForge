#!/usr/bin/env python3
"""Convert a LOBSTER message file into TickForge's CSV event format.

LOBSTER (https://lobsterdata.com) publishes NASDAQ ITCH-derived limit order
book data as paired CSV files per symbol-day:

    {ticker}_{date}_{start}_{end}_message_{level}.csv
    {ticker}_{date}_{start}_{end}_orderbook_{level}.csv

This script reads the *message* file and emits a TickForge event CSV. The
*orderbook* file is the published final-state snapshot you can use as a
correctness oracle against TickForge's reconstructed book.

LOBSTER message schema (no header in the raw file):

    time, type, order_id, size, price, direction

    type code -> meaning
    1  ADD       (submission of a new limit order)
    2  CANCEL    (partial deletion -- size reduction)
    3  CANCEL    (deletion -- order fully removed)
    4  TRADE     (execution of a visible limit order)
    5  TRADE     (execution of a hidden limit order)
    6  CROSS     (cross trade -- opening/closing auction)
    7  HALT      (trading halt indicator)

    direction -> 1 = BUY, -1 = SELL

This script maps LOBSTER -> TickForge as follows:

    1  -> ADD
    2  -> MODIFY (synthesised: same order_id, reduced size)
    3  -> CANCEL
    4  -> TRADE
    5,6,7 -> skipped with a warning (hidden / auction / halt are out of
            scope for TickForge's L3 displayed-book replay)

LOBSTER prices are floats in dollars; TickForge uses signed 64-bit
integers. This script multiplies by --price-scale (default 100, so
$101.2345 -> 10123450 if you also multiply LOBSTER's published scale
of 10000 -> use --price-scale 10000 for exact cents).

LICENSE NOTE
============
LOBSTER provides academic-use samples free of charge. Do NOT commit
raw LOBSTER CSVs to a public repository -- their license restricts
redistribution. Convert locally, replay with TickForge, and commit
only the summary or this adapter script. Add the raw LOBSTER files
to .gitignore.
"""

from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


@dataclass
class ActiveOrder:
    order_id: int
    side: str
    price: int
    size: int


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Convert a LOBSTER message CSV to TickForge event CSV."
    )
    p.add_argument("--input", type=Path, required=True, help="LOBSTER message CSV")
    p.add_argument("--output", type=Path, required=True, help="output TickForge CSV")
    p.add_argument(
        "--price-scale",
        type=int,
        default=10000,
        help="multiplier applied to LOBSTER price (default 10000; LOBSTER prices are "
             "already in 10000ths of a dollar in the raw integer column, so 10000 keeps "
             "them as-is)",
    )
    p.add_argument(
        "--time-base-ns",
        type=int,
        default=0,
        help="optional ns offset added to each timestamp_ns",
    )
    p.add_argument(
        "--initial-book",
        type=Path,
        default=None,
        help="path to the matching LOBSTER orderbook CSV; if given, the first row is "
             "read and emitted as synthetic ADD events (one per occupied level per "
             "side) at timestamp 0 with synthetic high-numbered order ids, so the "
             "pre-session resting depth is present before the message stream replays. "
             "Combine with --allow-orphan-events on tickforge_replay so cancels/trades "
             "against unknown ids reduce that seeded depth.",
    )
    p.add_argument(
        "--seed-id-base",
        type=int,
        default=10**15,
        help="starting order id used for synthetic seed orders; must be larger than "
             "any real LOBSTER order id in the captured day",
    )
    p.add_argument(
        "--strict",
        action="store_true",
        help="abort on any malformed row instead of skipping",
    )
    return p.parse_args()


LOBSTER_SENTINEL = 9_999_999_999


def read_initial_book(path: Path) -> tuple[list[tuple[int, int]], list[tuple[int, int]]]:
    """Read row 0 of a LOBSTER orderbook CSV and return (bids, asks).

    LOBSTER orderbook columns repeat every 4: ASK_PRICE_i, ASK_SIZE_i,
    BID_PRICE_i, BID_SIZE_i. Unoccupied levels are filled with sentinel
    +/- 9_999_999_999 / 0 and are skipped here.
    """
    with path.open() as f:
        first = next((line for line in f if line.strip()), None)
    if first is None:
        raise SystemExit(f"empty orderbook file: {path}")

    fields = [int(x) for x in first.strip().split(",")]
    if len(fields) % 4 != 0:
        raise SystemExit(
            f"orderbook row has {len(fields)} columns; expected multiple of 4"
        )

    asks: list[tuple[int, int]] = []
    bids: list[tuple[int, int]] = []
    for i in range(len(fields) // 4):
        a_p, a_s, b_p, b_s = fields[4 * i : 4 * i + 4]
        if abs(a_p) != LOBSTER_SENTINEL and a_s > 0:
            asks.append((a_p, a_s))
        if abs(b_p) != LOBSTER_SENTINEL and b_s > 0:
            bids.append((b_p, b_s))
    return bids, asks


def seconds_to_ns(seconds_field: str) -> int:
    # LOBSTER times are fractional seconds since midnight, microsecond resolution.
    seconds = float(seconds_field)
    return int(round(seconds * 1_000_000_000))


def main() -> int:
    args = parse_args()
    if not args.input.exists():
        print(f"error: input file not found: {args.input}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)

    active: dict[int, ActiveOrder] = {}
    n_written = 0
    n_skipped = 0
    n_seeded = 0
    skipped_breakdown: dict[str, int] = {}

    with args.input.open(newline="") as src, args.output.open("w", newline="") as dst:
        reader = csv.reader(src)
        writer = csv.writer(dst)
        writer.writerow(["timestamp_ns", "type", "order_id", "side", "price", "quantity"])

        skip_first_messages = 0
        if args.initial_book is not None:
            seed_bids, seed_asks = read_initial_book(args.initial_book)
            next_seed_id = args.seed_id_base
            # Emit one synthetic ADD per occupied level, asks ascending then
            # bids descending, all at timestamp 0 so they precede every real
            # event. These do NOT enter `active` because real LOBSTER cancel/
            # trade events for pre-session orders use the real (unknown to us)
            # ids; with --allow-orphan-events on the replay side, those events
            # will drain the seeded depth directly.
            for price, size in seed_asks:
                writer.writerow([0, "ADD", next_seed_id, "SELL", price, size])
                next_seed_id += 1
                n_seeded += 1
            for price, size in seed_bids:
                writer.writerow([0, "ADD", next_seed_id, "BUY", price, size])
                next_seed_id += 1
                n_seeded += 1

            # LOBSTER's orderbook row 0 reflects the book state AFTER message 0
            # has been applied. To avoid double-counting message 0 we skip it
            # in the replay; from message 1 onwards, replay tracks LOBSTER's
            # snapshots one-for-one (subject to LOBSTER's level-N message
            # filtering described in the findings report).
            skip_first_messages = 1

        msg_index = -1  # incremented BEFORE processing so messages are 0-indexed
        for line_no, row in enumerate(reader, start=1):
            if not row or all(not c.strip() for c in row):
                continue
            msg_index += 1
            if msg_index < skip_first_messages:
                continue
            if len(row) < 6:
                msg = f"line {line_no}: expected 6 LOBSTER columns, got {len(row)}"
                if args.strict:
                    print(msg, file=sys.stderr)
                    return 1
                n_skipped += 1
                skipped_breakdown["malformed"] = skipped_breakdown.get("malformed", 0) + 1
                continue

            try:
                ts_ns = seconds_to_ns(row[0]) + args.time_base_ns
                lob_type = int(row[1])
                order_id = int(row[2])
                size = int(row[3])
                price_int = int(row[4])  # LOBSTER raw price is integer ticks
                direction = int(row[5])
            except ValueError as ex:
                msg = f"line {line_no}: parse error: {ex}"
                if args.strict:
                    print(msg, file=sys.stderr)
                    return 1
                n_skipped += 1
                skipped_breakdown["parse_error"] = skipped_breakdown.get("parse_error", 0) + 1
                continue

            side = "BUY" if direction == 1 else "SELL"
            price = price_int  # already integer; --price-scale applied below if not 10000
            if args.price_scale != 10000:
                # LOBSTER raw integer is 10000ths-of-a-dollar; rescale if requested.
                price = (price_int * args.price_scale) // 10000

            tf_type: Optional[str] = None
            tf_qty = size

            if lob_type == 1:
                tf_type = "ADD"
                active[order_id] = ActiveOrder(order_id, side, price, size)
            elif lob_type == 2:
                # Partial deletion. If we know the order, emit MODIFY with the
                # remaining size. If we don't (pre-session order), emit a
                # CANCEL with the cancelled size so the orphan-event fallback
                # in OrderBook subtracts that depth from the seeded level.
                existing = active.get(order_id)
                if existing is None:
                    tf_type = "CANCEL"
                    tf_qty = size  # depth to drain at (side, price)
                    skipped_breakdown["orphan_partial_cancel_emitted"] = (
                        skipped_breakdown.get("orphan_partial_cancel_emitted", 0) + 1
                    )
                else:
                    new_size = max(0, existing.size - size)
                    if new_size == 0:
                        tf_type = "CANCEL"
                        tf_qty = existing.size  # carries the size for orphan fallback if engine drops it
                        active.pop(order_id, None)
                    else:
                        tf_type = "MODIFY"
                        tf_qty = new_size
                        existing.size = new_size
            elif lob_type == 3:
                # Full deletion. LOBSTER's `size` field is the order's size at
                # removal -- carry it through so the orphan fallback knows how
                # much depth to drain when the order id is unknown to TickForge.
                tf_type = "CANCEL"
                tf_qty = size
                active.pop(order_id, None)
            elif lob_type == 4:
                tf_type = "TRADE"
                tf_qty = size
                existing = active.get(order_id)
                if existing is not None:
                    existing.size -= size
                    if existing.size <= 0:
                        active.pop(order_id, None)
            else:
                # 5 = hidden execution, 6 = cross trade, 7 = halt -- not modelled.
                n_skipped += 1
                key = f"lobster_type_{lob_type}"
                skipped_breakdown[key] = skipped_breakdown.get(key, 0) + 1
                continue

            writer.writerow([ts_ns, tf_type, order_id, side, price, tf_qty])
            n_written += 1

    print(f"wrote {n_written} TickForge events to {args.output}")
    if n_seeded:
        print(f"  including {n_seeded} synthetic seed ADD events from --initial-book")
    if n_skipped:
        print(f"skipped {n_skipped} rows:")
    if skipped_breakdown:
        for k, v in sorted(skipped_breakdown.items()):
            print(f"  {k}: {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
