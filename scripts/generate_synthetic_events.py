#!/usr/bin/env python3
"""Generate deterministic synthetic TickForge market events."""

from __future__ import annotations

import argparse
import csv
import random
from dataclasses import dataclass
from pathlib import Path


@dataclass
class ActiveOrder:
    order_id: int
    side: str
    price: int
    quantity: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate synthetic TickForge CSV events.")
    parser.add_argument("--events", type=int, required=True, help="number of events to write")
    parser.add_argument("--output", type=Path, required=True, help="output CSV path")
    parser.add_argument("--seed", type=int, default=42, help="deterministic RNG seed")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.events < 0:
        raise SystemExit("--events must be non-negative")

    rng = random.Random(args.seed)
    active: list[ActiveOrder] = []
    next_order_id = 1
    timestamp_ns = 1_000_000_000

    args.output.parent.mkdir(parents=True, exist_ok=True)

    with args.output.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp_ns", "type", "order_id", "side", "price", "quantity"])

        for _ in range(args.events):
            op = rng.random()

            if not active or op < 0.55:
                side = "BUY" if rng.random() < 0.5 else "SELL"
                price = rng.randint(9_950, 10_000) if side == "BUY" else rng.randint(10_005, 10_055)
                quantity = rng.randint(1, 100)
                order = ActiveOrder(next_order_id, side, price, quantity)
                next_order_id += 1
                active.append(order)
                writer.writerow([timestamp_ns, "ADD", order.order_id, side, price, quantity])
            else:
                index = rng.randrange(len(active))
                order = active[index]

                if op < 0.70:
                    candidate_price = max(1, order.price + rng.randint(-3, 3))
                    if order.side == "BUY":
                        order.price = min(10_000, candidate_price)
                    else:
                        order.price = max(10_005, candidate_price)
                    order.quantity = rng.randint(1, 100)
                    writer.writerow(
                        [
                            timestamp_ns,
                            "MODIFY",
                            order.order_id,
                            order.side,
                            order.price,
                            order.quantity,
                        ]
                    )
                elif op < 0.85:
                    traded_quantity = rng.randint(1, order.quantity)
                    writer.writerow(
                        [
                            timestamp_ns,
                            "TRADE",
                            order.order_id,
                            order.side,
                            order.price,
                            traded_quantity,
                        ]
                    )
                    order.quantity -= traded_quantity
                    if order.quantity == 0:
                        active.pop(index)
                else:
                    writer.writerow(
                        [timestamp_ns, "CANCEL", order.order_id, order.side, order.price, 0]
                    )
                    active.pop(index)

            timestamp_ns += rng.randint(1, 1_000)

    print(f"wrote {args.events} events to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
