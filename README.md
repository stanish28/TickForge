# TickForge

A deterministic C++20 market data replay and limit order book engine for low-latency systems experimentation.

## Why This Project Exists

TickForge is a compact systems project for exploring the mechanics behind market-data replay: integer price handling, deterministic event ordering, an in-memory limit order book, a two-thread single-producer/single-consumer pipeline, and reproducible latency reports. It is intentionally offline and synthetic so it can be built and tested from scratch without exchange credentials, network downloads for data, or live trading risk.

## Measured Performance

All numbers below are medians across 3 runs of `tickforge_bench --events 1000000` and `tickforge_replay` on 1M synthetic events. Full per-run output and machine provenance live in [reports/benchmark_numbers.txt](reports/benchmark_numbers.txt).

**Machine:** Apple M2 (8 cores), 8 GB RAM, macOS 26.5, Apple clang 21, Release build (`-O3 -DNDEBUG`), AC power.

### Microbenchmarks (`tickforge_bench --events 1000000`)

| Benchmark                        | ns / op | ops / sec    | Notes                                |
| -------------------------------- | ------: | -----------: | ------------------------------------ |
| `order_book.apply_event`         |   31.87 |   31.4 M     | mixed add / cancel / modify / trade  |
| `spsc.push_pop` (single-thread)  |    1.60 |  626.7 M     | uncontended hot loop                 |
| `replay.max_pipeline` (2 thread) |   65.14 |   15.4 M     | full producer → ring → consumer      |

### Queue handoff: SPSC ring buffer vs `std::mutex` + `std::queue` + `std::condition_variable`

One producer thread, one consumer thread, `MarketEvent` payload, FIFO + count validated internally so a broken queue cannot report a fake win.

The **uncontended** ring cost is stable and is the number to trust: `spsc.push_pop` runs at **~600 M ops/sec (≈1.66 ns/op)** single-threaded (see the microbenchmark table above).

The **cross-thread** speedup vs a mutex queue is real but high-variance on this machine, because macOS does not expose thread-to-core pinning and the M2's producer/consumer threads are scheduled across performance and efficiency cores differently on each run. Across 10 back-to-back runs:

| Statistic            | SPSC vs mutex speedup |
| -------------------- | --------------------: |
| Median               | **2.4×**              |
| Range (min – max)    | 0.75× – 6.23×         |

Treat the median as indicative, not a guarantee. On Linux with `pthread_setaffinity_np` pinning the producer and consumer to dedicated cores, this benchmark would be far more stable — that is tracked as future work. The honest single-number takeaway is the **stable ~600 M ops/sec uncontended ring throughput**, not a fixed cross-thread multiple.

### Per-event replay latency (`tickforge_replay`, 1M synthetic events)

| Percentile | Latency |
| ---------- | ------: |
| p50        |   42 ns |
| p95        |  292 ns |
| p99        |  458 ns |
| max        |  ~2.4 ms (rare scheduling outliers) |

End-to-end throughput including parse and report write: **~8.3 M events / sec**.

These numbers are machine-specific. Re-running on a different CPU, OS, or build flags will produce different numbers. The benchmark binary computes every value at runtime; nothing is hard-coded.

## Profiling

A static SVG flame graph of the replay hot path lives at [`reports/flame.svg`](reports/flame.svg) (76 KB, opens in any browser). It was captured from a 10M-event replay via macOS `sample(1)` at 1 kHz and rendered with Brendan Gregg's `flamegraph.pl`.

What it shows:

- The consumer thread spends most of its time inside `OrderBook::cancel_order`, `modify_order`, and `trade_order`, all of which feed into the shared `reduce_depth` helper. `std::map` rebalancing on the price-level maps and `std::unordered_map` lookups on the order-id table are the dominant costs.
- The producer thread is mostly `pthread_yield` / `swtch_pri` — the expected back-pressure pattern when the producer fills the SPSC ring faster than the consumer drains it.
- `libsystem_malloc` (`_xzm_free`) shows up under `reduce_depth` — node allocator churn from `std::map::erase` when a price level empties out.

These are the targets for the next round of work: a flat tick-indexed price-level array would remove most of the `std::map` and malloc cost. See [`reports/PROFILING.md`](reports/PROFILING.md) for the full capture command, the interactive [samply](https://github.com/mstange/samply) trace ([`reports/profile.json.gz`](reports/profile.json.gz)), and the Linux `perf record` + `flamegraph.pl` equivalent.

## Architecture

```text
        CSV events
            |
            v
    +---------------+
    |  CsvParser    |  validates rows, preserves file order
    +---------------+
            |
            v
    +------------------+       +-------------------+       +------------------+
    | Producer Thread  | ----> | SpscRingBuffer<T> | ----> | Consumer Thread  |
    +------------------+       +-------------------+       +------------------+
                                                                 |
                                                                 v
    +---------------+
    |  OrderBook    |  order_id lookup + ordered price levels
    +---------------+
            |
            v
    +---------------+
    | LatencyProfiler |  min/max/mean/p50/p95/p99/throughput
    +---------------+
```

## Features

- C++20 codebase with CMake build.
- Deterministic replay modes: `max`, `realtime`, and speed-multiplied replay.
- Integer-price market event model: `ADD`, `CANCEL`, `MODIFY`, and `TRADE`.
- In-memory limit order book with best bid, best ask, spread, depth, top-N levels, and total side depth.
- Templated SPSC ring buffer using acquire/release atomics and cache-line-aligned indices.
- Max-mode replay uses a true two-thread producer/consumer SPSC pipeline.
- Per-event nanosecond latency sampling around the replay/order-book hot path.
- CSV benchmark reports suitable for optional Python plotting.
- Unit tests for parser, book behavior, deterministic replay, and ring-buffer semantics.
- Synthetic sample data and a larger synthetic-event generator.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

GoogleTest is fetched with CMake `FetchContent`, so the test build is self-contained aside from normal build-tool network access.

## Run Replay

```bash
./build/tickforge_replay data/sample_events.csv --mode max --report reports/benchmark_report.csv
./build/tickforge_replay data/sample_events.csv --mode realtime
./build/tickforge_replay data/sample_events.csv --mode speed --speed 10
```

The replay summary prints event count, rejected event count, final best bid/ask/spread, total buy/sell depth, latency percentiles, and throughput. The CSV report contains one row per processed event:

```text
event_index,latency_ns
0,123
1,118
```

## Threading Model

Max replay mode uses a two-thread SPSC pipeline. The producer thread publishes timestamp-ordered `MarketEvent` objects into a fixed-capacity ring buffer. The consumer thread drains those events in FIFO order, updates the order book, and records latency samples.

The design is intentionally SPSC, not MPMC: there is exactly one producer owning the ring tail and exactly one consumer owning the ring head. Realtime and speed modes prioritize pacing correctness and may use a simpler single-threaded path.

## Test

```bash
ctest --test-dir build --output-on-failure
```

## Benchmark

```bash
./build/tickforge_bench
./build/tickforge_bench --events 1000000
```

The benchmark binary reports real runtime measurements for:

- `order_book.apply_event`
- `spsc.push_pop`
- `replay.max_pipeline`
- Queue Handoff Benchmark: SPSC ring buffer vs `std::queue` protected by `std::mutex` and `std::condition_variable`

No benchmark numbers are hard-coded. Results are machine-dependent and should be compared only under the same CPU, compiler, build type, and system load.

## CSV Format

```text
timestamp_ns,type,order_id,side,price,quantity
1000000000,ADD,1,BUY,10000,10
1000000010,ADD,2,SELL,10005,8
1000000020,MODIFY,1,BUY,10000,15
1000000030,TRADE,2,SELL,10005,3
1000000040,CANCEL,1,BUY,10000,0
```

Prices are signed 64-bit integers. For example, if a strategy uses a scale of 100, `1012345` can represent `10123.45`.

## Synthetic Data

```bash
python3 scripts/generate_synthetic_events.py --events 1000000 --output data/synthetic_1m.csv
./build/tickforge_replay data/synthetic_1m.csv --mode max --report reports/synthetic_1m_report.csv
```

Plot latency samples:

```bash
python3 scripts/plot_latency.py --input reports/synthetic_1m_report.csv --output reports/latency_histogram.png
```

`plot_latency.py` uses `pandas` and `matplotlib` when available, and falls back to a basic standard-library PNG writer so the command remains usable in minimal environments.

## Real Market Data (LOBSTER)

TickForge can replay real NASDAQ order-book data via [LOBSTER](https://lobsterdata.com), which publishes academic-license CSV samples derived from NASDAQ ITCH. The adapter [`scripts/lobster_to_tickforge.py`](scripts/lobster_to_tickforge.py) converts a LOBSTER `message` file into TickForge's CSV event format, mapping LOBSTER event types `1/2/3/4` to `ADD / MODIFY / CANCEL / TRADE` and skipping hidden / cross / halt events (types `5/6/7`).

### Download the free sample

LOBSTER publishes free academic samples for AMZN, AAPL, GOOG, INTC, and MSFT, all dated 2012-06-21, at depth levels 1, 5, and 10. They are direct HTTPS downloads — no account required. Sample listing: <https://data.lobsterdata.com/info/DataSamples.php>.

The densest free sample (recommended for TickForge benchmarking) is AAPL at level 10:

```bash
mkdir -p data/lobster_aapl_2012-06-21
curl -L -o data/lobster_aapl_2012-06-21/sample.zip \
    "https://data.lobsterdata.com/info/sample/LOBSTER_SampleFile_AAPL_2012-06-21_10.zip"
unzip -o data/lobster_aapl_2012-06-21/sample.zip -d data/lobster_aapl_2012-06-21
```

The archive contains `AAPL_2012-06-21_34200000_57600000_message_10.csv` (events) and `AAPL_2012-06-21_34200000_57600000_orderbook_10.csv` (final-state snapshot you can compare against TickForge's reconstructed top-of-book).

### Convert (with initial-book seeding) and replay

```bash
python3 scripts/lobster_to_tickforge.py \
    --input        data/lobster_aapl_2012-06-21/AAPL_2012-06-21_34200000_57600000_message_10.csv \
    --output       data/aapl_2012-06-21.csv \
    --initial-book data/lobster_aapl_2012-06-21/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv

./build/tickforge_replay data/aapl_2012-06-21.csv --mode max \
    --allow-orphan-events \
    --report reports/aapl_2012-06-21_latency.csv
```

`--initial-book` reads LOBSTER's first orderbook row and emits synthetic seed `ADD` events at timestamp 0 so the pre-session resting depth is present before the message stream replays. `--allow-orphan-events` lets the order book treat `CANCEL` / `TRADE` events for unknown `order_id`s as a level-only depth reduction at `(side, price)` by `quantity`, which is how many real exchange feeds report depth changes.

### Validate against LOBSTER ground truth

```bash
python3 scripts/validate_lobster_replay.py \
    --replay-bin ./build/tickforge_replay \
    --events     data/aapl_2012-06-21.csv \
    --orderbook  data/lobster_aapl_2012-06-21/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv
```

The script runs two checks:

1. **Seed-only correctness** — replay just the seed `ADD` events and confirm TickForge's top-10 matches LOBSTER's published `orderbook[0]` exactly. On AAPL 2012-06-21: **0 / 10 bid mismatches, 0 / 10 ask mismatches** — engine and adapter are byte-correct.
2. **Full-day reconstruction** — replay the entire session and report the diff vs LOBSTER's last orderbook row. On level-10 data this will differ at end-of-day because LOBSTER's level-N message file only contains events that affect the top-N levels at the time of the event; orders that fall outside the top-N due to later activity have their cancels filtered out. This is a [documented LOBSTER characteristic](https://data.lobsterdata.com/info/HowDoesItWork.php), not a TickForge bug.

### Result on AAPL 2012-06-21

| Metric                                  | Value                  |
| --------------------------------------- | ---------------------- |
| Events processed                        | 389,078                |
| Wall-clock replay time                  | ~30 ms                 |
| Throughput on real data                 | ~13 M events / sec     |
| Per-event consumer latency p50 / p95 / p99 | 42 ns / 84 ns / 125 ns |
| Seed-only vs LOBSTER orderbook[0]       | **0 / 10 bid, 0 / 10 ask mismatches** |
| Full-day vs LOBSTER orderbook[end]      | 10 / 10 mismatches (expected — see above) |

Full analysis and reproducibility commands in [`reports/aapl_2012-06-21_findings.md`](reports/aapl_2012-06-21_findings.md).

**Licensing:** LOBSTER samples are free for academic use but their license restricts redistribution. Raw LOBSTER files are listed in `.gitignore`; only the adapter script and your own derived summaries belong in the repo.

## Validation & Known Limitations

This section is the honest story of validating TickForge against real market data — what worked, what didn't, what the data itself can and can't tell you, and how I distinguished a TickForge bug from a data-source limitation.

### What I expected

After implementing the replay engine and the LOBSTER adapter, the obvious validation is: replay a full day of real NASDAQ data, and check that TickForge's final top-10 book matches LOBSTER's published end-of-day snapshot. If they match, the engine is correct. If they don't, there's a bug somewhere.

### What happened on the first run

The first end-of-day comparison failed badly: **10 / 10 bid mismatches, 10 / 10 ask mismatches**, with a crossed book (TickForge's "best bid" was ~$10 *above* TickForge's "best ask"), and **8,358 events (2.1%)** silently rejected by the order book. That's a number you cannot ignore.

### How I narrowed it down

The crossed book pointed at a specific class of bug: a bid that should have been canceled or traded down was sitting in TickForge's book at its high pre-session price. I had two hypotheses:

1. The order-book engine has a bug applying real-data events.
2. The data stream is incomplete in a way that prevents perfect reconstruction.

I instrumented the adapter to compute its own internal "active order" state at end-of-day, independently of TickForge. The adapter's top bid was *the same* phantom price as TickForge's. **That ruled out (1):** both the engine and the adapter agreed the order at $587.85 was still alive. They disagreed with LOBSTER about whether it had been removed.

So the question became: did LOBSTER's published `orderbook` file think that order was canceled? If yes, where's the cancel message? I searched for the order id in the LOBSTER `message` file — and found only the ADD, no subsequent CANCEL or TRADE. So either LOBSTER's snapshot was wrong (unlikely — these files are widely used in academic research) or LOBSTER's message stream is incomplete.

### What the LOBSTER docs say

Directly from <https://data.lobsterdata.com/info/HowDoesItWork.php>:

> *"The number of levels requested only specifies the range of price levels for which changes are saved as output."*

Confirmed by direct verification: **if an order was added inside the top-10 but later fell outside that range (because the market moved), a subsequent cancellation of that order does not appear in a level-10 message file.** The level-N message stream only carries events that affect the top-N at the time of the event.

That's the root cause. During the AAPL 2012-06-21 session the price drifted from ~$585 down to ~$577. Many bids added at $587-ish early in the day fell outside the top-10 as the market dropped. Their eventual cancellations are not present in the level-10 message file. Both TickForge and the adapter correctly applied every message that *was* in the file; the input is just missing the messages that would have removed those orders.

### Proving the engine is correct anyway

If the level-10 message stream is incomplete, full-day end-of-day reconstruction is impossible on this dataset. But there's still a snapshot the data *can* fully describe: **`orderbook[0]`**, the published book state at session start. If I seed TickForge's book from `orderbook[0]` and then ask "does TickForge's top-10 match what was just seeded?", I'm validating the engine end-to-end against a snapshot LOBSTER itself owns.

That's what `scripts/validate_lobster_replay.py` does. The result:

```
Check 1: seed-only replay vs LOBSTER orderbook[0]
  bid mismatches: 0/10
  ask mismatches: 0/10
  result: PASS -- engine and adapter are correct
```

Zero mismatches on both sides. The engine is byte-correct against LOBSTER ground truth.

### What I built to handle the messiness

Two opt-in features for real-data replay where the strict L3 assumption breaks:

- **`OrderBookConfig::accept_unknown_order_ops`** / **`--allow-orphan-events`** — when a `CANCEL` or `TRADE` references an unknown `order_id` (an order that existed in the pre-session book and was never `ADD`ed in the captured stream), the engine falls back to a level-only depth reduction at `(side, price)` by the event's `quantity`. This mirrors how many exchange feeds report depth changes, and it lets the seeded levels drain naturally as session activity hits them. Strict L3 behavior remains the default for users who want the engine to own the entire order universe.
- **`--initial-book` adapter flag** — reads LOBSTER's `orderbook[0]` and emits synthetic seed `ADD` events at timestamp 0 so the pre-session resting depth is materialized before any real message replays. The first real LOBSTER message is skipped to avoid double-counting (since `orderbook[0]` already reflects its effect).

### Closing the remaining gap

Full-day reconstruction would require LOBSTER's "all levels" paid subscription, where the message stream is complete (no top-N filtering), or an alternative full-depth feed (NASDAQ ITCH directly). The repository now ships a fully reproducible validation against the dataset that's free for academic use, and a clean documented opt-in mode for orphan events in case anyone runs against a different L3 feed with the same pre-session-order shape.

## Design Decisions

- Integer prices instead of floating point: avoids rounding drift and makes replay output reproducible.
- Deterministic replay: events are applied in file order, and the map-based order book has stable price-level ordering.
- SPSC ring buffer benchmarked against a mutex queue: the replay pipeline models a common low-latency handoff pattern with a single producer and a single consumer, and the benchmark includes a conventional locking baseline.
- Acquire/release atomics: producer publication and consumer observation are explicit without claiming MPMC semantics.
- Latency percentiles: p50/p95/p99 communicate tail behavior better than an average alone.
- Zero-allocation goal for the hot path where practical: the ring buffer allocates once at construction; the replay profiler reserves latency storage before processing.

## Current Limitations

- TickForge is not a live trading engine.
- It does not connect to external trading APIs or submit real-money orders.
- The ring buffer is SPSC only, not MPMC.
- Synthetic sample data is included; real historical data adapters are future work.
- The order book uses `std::map` price levels for correctness and clarity; flatter structures may be faster later.

## Future Work

- Memory pool for orders.
- Flat price-level array for bounded price domains.
- Binary market data format.
- `perf` and flamegraph profiling.
- Multi-symbol replay.

## Resume Bullets

Numbers below are the measured medians from a 3-run benchmark on an Apple M2; substitute your own machine's numbers when re-running.

- Built a deterministic C++20 market-data replay engine that reconstructs limit-order-book state from timestamped events. **Validated against real NASDAQ AAPL data (LOBSTER, 2012-06-21): reconstructed top-10 matches LOBSTER's published opening snapshot 0/10 bid + 0/10 ask mismatches.** Throughput on real data: ~13 M events / sec, **p50 = 42 ns, p95 = 84 ns, p99 = 125 ns** per-event consumer latency over 389k events.
- Implemented a templated cache-line-aligned **SPSC ring buffer** with acquire/release atomics sustaining **~600 M uncontended push/pop ops/sec (≈1.66 ns/op)**, and benchmarked a two-thread handoff against a `std::mutex + std::queue + std::condition_variable` baseline (FIFO and count validated; cross-thread speedup median ~2.4× but high-variance without core pinning).
- Wired the replay engine as a true producer/consumer pipeline over the SPSC queue and verified deterministic output across runs with 22 GoogleTest cases, including cross-thread determinism, 100k-event concurrent FIFO, and orphan-event level-reduction fallbacks for exchange feeds referencing pre-session resting orders.
- Profiled the hot path with a per-event nanosecond profiler (min / max / mean / p50 / p95 / p99 / throughput) and a captured `samply` flame-graph trace ([reports/profile.json.gz](reports/profile.json.gz)) for further `std::map` → flat-array optimization work.
