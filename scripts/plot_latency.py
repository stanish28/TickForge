#!/usr/bin/env python3
"""Plot a latency histogram from a TickForge per-event latency CSV report."""

from __future__ import annotations

import argparse
import csv
import struct
import zlib
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Plot TickForge latency histogram.")
    parser.add_argument("--input", type=Path, required=True, help="input benchmark_report.csv")
    parser.add_argument("--output", type=Path, required=True, help="output PNG path")
    parser.add_argument("--bins", type=int, default=80, help="histogram bin count")
    return parser.parse_args()


def load_latencies_stdlib(path: Path) -> list[int]:
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if "latency_ns" not in (reader.fieldnames or []):
            raise SystemExit("input CSV must contain a latency_ns column")
        return [int(row["latency_ns"]) for row in reader if row.get("latency_ns")]


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + kind
        + data
        + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    )


def write_simple_png(path: Path, latencies: list[int], bins: int) -> None:
    if not latencies:
        raise SystemExit("input CSV contains no latency samples")

    width = 1000
    height = 600
    margin_left = 70
    margin_right = 30
    margin_top = 40
    margin_bottom = 70
    plot_width = width - margin_left - margin_right
    plot_height = height - margin_top - margin_bottom

    pixels = bytearray([255, 255, 255] * width * height)

    def fill_rect(x0: int, y0: int, x1: int, y1: int, color: tuple[int, int, int]) -> None:
        x0 = max(0, min(width, x0))
        x1 = max(0, min(width, x1))
        y0 = max(0, min(height, y0))
        y1 = max(0, min(height, y1))
        for y in range(y0, y1):
            row = y * width * 3
            for x in range(x0, x1):
                offset = row + x * 3
                pixels[offset : offset + 3] = bytes(color)

    min_latency = min(latencies)
    max_latency = max(latencies)
    bin_count = max(1, bins)
    counts = [0] * bin_count

    if min_latency == max_latency:
        counts[0] = len(latencies)
    else:
        span = max_latency - min_latency
        for latency in latencies:
            index = min(bin_count - 1, int((latency - min_latency) * bin_count / span))
            counts[index] += 1

    max_count = max(counts)

    axis_color = (50, 50, 50)
    grid_color = (230, 230, 230)
    bar_color = (31, 119, 180)

    for i in range(6):
        y = margin_top + int(i * plot_height / 5)
        fill_rect(margin_left, y, width - margin_right, y + 1, grid_color)

    fill_rect(margin_left, margin_top, margin_left + 2, height - margin_bottom, axis_color)
    fill_rect(margin_left, height - margin_bottom, width - margin_right, height - margin_bottom + 2, axis_color)

    for i, count in enumerate(counts):
        bar_height = 0 if max_count == 0 else int(count * (plot_height - 1) / max_count)
        x0 = margin_left + int(i * plot_width / bin_count)
        x1 = margin_left + int((i + 1) * plot_width / bin_count)
        y0 = height - margin_bottom - bar_height
        y1 = height - margin_bottom
        fill_rect(x0 + 1, y0, max(x0 + 2, x1 - 1), y1, bar_color)

    raw_rows = bytearray()
    for y in range(height):
        raw_rows.append(0)
        start = y * width * 3
        raw_rows.extend(pixels[start : start + width * 3])

    png = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(bytes(raw_rows), level=9))
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def plot_with_pandas_matplotlib(args: argparse.Namespace) -> bool:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import pandas as pd
    except ImportError:
        return False

    df = pd.read_csv(args.input)
    if "latency_ns" not in df.columns:
        raise SystemExit("input CSV must contain a latency_ns column")

    latencies = df["latency_ns"].dropna()
    if latencies.empty:
        raise SystemExit("input CSV contains no latency samples")

    args.output.parent.mkdir(parents=True, exist_ok=True)

    fig, ax = plt.subplots(figsize=(10, 6))
    ax.hist(latencies, bins=args.bins, color="#1f77b4", edgecolor="white", linewidth=0.5)
    ax.set_title("TickForge per-event processing latency")
    ax.set_xlabel("Latency (ns)")
    ax.set_ylabel("Event count")
    ax.grid(True, axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(args.output, dpi=150)
    print(f"wrote latency histogram to {args.output}")
    return True


def main() -> int:
    args = parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if plot_with_pandas_matplotlib(args):
        return 0

    latencies = load_latencies_stdlib(args.input)
    write_simple_png(args.output, latencies, args.bins)
    print(
        "pandas/matplotlib not available; wrote a basic stdlib PNG histogram "
        f"to {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
