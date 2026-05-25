# Profiling TickForge

The repository ships two profiling artifacts:

- **[`reports/flame.svg`](flame.svg)** — static flame graph SVG (76 KB, opens in any browser). Captured from a 10M-event replay via macOS `sample(1)` at 1 kHz, converted with Brendan Gregg's `stackcollapse-sample.awk` and rendered with `flamegraph.pl`.
- **[`reports/profile.json.gz`](profile.json.gz)** + symbol sidecar — interactive [samply](https://github.com/mstange/samply) trace of `tickforge_bench --events 1000000`; drag into <https://profiler.firefox.com> for the click-through call-tree / flame-graph UI.

## What the flame graph shows

The consumer thread's hot path is dominated by the four `OrderBook` event handlers and their shared `reduce_depth` helper:

- `OrderBook::trade_order`, `cancel_order`, `modify_order` — dispatch from `apply_event` (called by the consumer in `ReplayEngine::replay_max_threaded`).
- `reduce_depth` — the common subroutine each handler calls to drain a price level. This frame is wide because every cancel and every level-leaving modify routes through it. `std::map::find` + erase on the level map is the dominant cost inside.
- `std::__hash_table` lookups — `unordered_map<order_id, Order>` access for cancel/modify/trade.
- `libsystem_malloc` (`_xzm_free`, `_xzm_xzone_malloc_freelist_outlined`) — node allocator for `std::map` insert/erase when a level is created or destroyed.

The producer thread is dominated by `libsystem_kernel`swtch_pri` (i.e. `pthread_yield`), which is the expected back-pressure pattern when the producer fills the ring faster than the consumer drains it; `mach_continuous_time` shows up under the per-event latency timing call site.

**Highest-leverage next optimizations, ordered by expected impact:**

1. Replace `std::map` price levels with a flat array indexed by `(price - reference_tick) / tick_size`. Eliminates the `_xzm_free` / `_xzm_xzone_malloc_freelist_outlined` calls and the tree-rebalance work. Expected ~2–3× on order-book throughput.
2. Replace `std::unordered_map<order_id, Order>` with a custom open-addressing hash or a slab allocator keyed by order id. `__hash_table` allocations show up across cancel/modify/trade.
3. Batch the latency timing — `mach_continuous_time` is a syscall (~25 ns on M2). Sampling every Nth event halves clock-call overhead at small accuracy cost.

## How the SVG was captured

## How the trace was captured

### Static SVG flame graph (`reports/flame.svg`)

```bash
# 1. Release build with debug symbols
cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-profile -j --target tickforge_replay

# 2. Generate a workload large enough to dominate wall time
python3 scripts/generate_synthetic_events.py --events 10000000 --output data/synthetic_10m.csv

# 3. Start the replay in background, capture stack samples with macOS sample(1)
./build-profile/tickforge_replay data/synthetic_10m.csv --mode max > /tmp/replay.out &
RPID=$!
sample $RPID 15 1 -file /tmp/tickforge_sample.txt -mayDie
wait $RPID

# 4. Collapse and render
brew install flamegraph  # provides stackcollapse-sample.awk + flamegraph.pl
stackcollapse-sample.awk /tmp/tickforge_sample.txt > /tmp/tickforge.folded
flamegraph.pl --title "TickForge replay hot path" --colors=java --width 1400 \
    /tmp/tickforge.folded > reports/flame.svg
```

The captured SVG covers 6.6M events of the consumer thread's hot path; producer-thread frames are visible separately in the same SVG (each thread becomes a stack-pile column).

### Interactive samply trace (`reports/profile.json.gz`)

- **Profiler:** [samply](https://github.com/mstange/samply) 0.13.1, a sampling CPU profiler. On macOS it uses the kernel sampling APIs; on Linux it can also import `perf.data`.
- **Command:**

  ```bash
  samply record --save-only --no-open --rate 999 \
      -o reports/profile.json.gz --unstable-presymbolicate \
      -- ./build-profile/tickforge_bench --events 1000000
  ```

- **Workload:** the full `tickforge_bench --events 1000000` run, which exercises:
  - `order_book.apply_event` (1M mixed events)
  - `spsc.push_pop` (1M push/pop on a single thread)
  - `replay.max_pipeline` (1M events through the two-thread producer/consumer pipeline)
  - SPSC vs mutex-queue handoff (1M events through each)
- **Machine:** Apple M2, 8 cores, macOS 26.5, Apple clang 21.

## How to view it

Drag `profile.json.gz` into <https://profiler.firefox.com>. The flame graph view is in the "Flame Graph" tab; "Call Tree" gives you top-down self-time. The profile already contains symbols so no rebuild is needed on the viewer's machine.

You can also load it locally:

```bash
samply load reports/profile.json.gz
```

This spins up a local web server and opens the same UI in the browser.

## How to regenerate

```bash
brew install samply           # macOS
# or: cargo install --locked samply

cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-profile -j --target tickforge_bench

samply record --save-only --no-open --rate 999 \
    -o reports/profile.json.gz --unstable-presymbolicate \
    -- ./build-profile/tickforge_bench --events 1000000
```

## Linux / `perf` equivalent

If you have a Linux box, the equivalent flow gets you a `.svg` flame graph through Brendan Gregg's scripts:

```bash
perf record -F 999 -g ./build/tickforge_bench --events 1000000
perf script > perf.out
stackcollapse-perf.pl perf.out > perf.folded
flamegraph.pl perf.folded > reports/flame.svg
```

Both artifacts answer the same question, just with different viewers.

## What to look at in the trace

If you open the trace, the consumer thread inside `replay.max_pipeline` and `run_queue_handoff_benchmark` is where almost all the interesting time lives. The frames to skim through, roughly in expected weight order:

- `tickforge::OrderBook::apply_event` — dispatch + branch into add/cancel/modify/trade.
- `tickforge::OrderBook::add_order` / `add_depth` — `std::unordered_map::emplace` and `std::map` rebalancing.
- `tickforge::OrderBook::reduce_depth` — `std::map::find` + erase, called by cancel/modify/trade.
- `tickforge::SpscRingBuffer<MarketEvent>::push` / `::pop` — should be a thin slice; if it's wide, the ring is contended or the consumer is starving.
- `std::this_thread::yield` — wide stripes here mean the consumer is spending real time spinning instead of doing work, which would be a signal to back off to a blocking strategy.

The two highest-leverage targets if you want to lower p99 further are (a) replacing the `std::map` price levels with a flat array indexed by tick offset and (b) replacing `std::unordered_map` for order lookup with a slab allocator. Both are listed under "Future Work" in the main README.
