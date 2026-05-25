# Flamegraph Notes

Flame graphs are useful for visualizing sampled stack traces and identifying hot code paths. TickForge does not require flamegraph generation to work automatically, but Linux `perf` can capture profiles for local investigation.

Build a release binary first:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Record a sampled profile:

```bash
perf record -F 99 -g ./build/tickforge_replay data/sample_events.csv --mode max --report reports/benchmark_report.csv
```

Inspect the profile interactively:

```bash
perf report
```

Export stack samples for flamegraph tooling:

```bash
perf script > reports/perf.out
```

If you have Brendan Gregg's FlameGraph scripts installed, a typical flow is:

```bash
stackcollapse-perf.pl reports/perf.out > reports/perf.folded
flamegraph.pl reports/perf.folded > reports/tickforge.svg
```

Use the resulting SVG to look for expensive call stacks in parsing, queue handoff, order-book mutation, and percentile/report generation.
