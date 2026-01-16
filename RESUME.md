# RESUME.md — Project Framing Guide

## Primary Resume Entry

---

**NASDAQ ITCH 5.0 Limit Order Book Replay Engine** | C++20, CMake, Google Benchmark, perf/flamegraphs, Python

- Engineered a zero-allocation limit order book replay pipeline in C++20 parsing NASDAQ TotalView-ITCH 5.0 binary feeds via mmap; achieved ≥5M events/sec sustained throughput with p99 latency under 400ns on 11GB historical data.
- Implemented cache-line-aligned slab allocator, Robin Hood open-addressing hash map, and fixed-range tick-array price ladder; validated correctness via deterministic replay checksums, libFuzzer corpus, and ASan/UBSan.

---

## Alternate Bullet Variants

Use these depending on which role you are targeting. Mix and match bullet 1 and bullet 2 independently.

### Bullet 1 — Emphasis Variants

**Throughput / systems angle** (default above — best for HFT/quant dev):
> Engineered a zero-allocation limit order book replay pipeline in C++20 parsing NASDAQ TotalView-ITCH 5.0 binary feeds via mmap; achieved ≥5M events/sec sustained throughput with p99 latency under 400ns on 11GB historical data.

**Microarchitecture angle** (best for low-latency infra roles):
> Built a cache-aware C++20 market data replay engine over NASDAQ ITCH 5.0; optimized memory layout via slab allocation and tick-array price ladder to achieve <2% L1 cache miss rate and zero hot-path heap allocations.

**Protocol/correctness angle** (best for exchange/market data infra roles):
> Implemented strict NASDAQ TotalView-ITCH 5.0 binary protocol parsing in C++20 with mmap zero-copy access, big-endian fixed-point price decoding, and deterministic replay producing bitwise-reproducible order book snapshots.

---

### Bullet 2 — Emphasis Variants

**Data structures / algorithms angle** (default above — broadly applicable):
> Implemented cache-line-aligned slab allocator, Robin Hood open-addressing hash map, and fixed-range tick-array price ladder; validated correctness via deterministic replay checksums, libFuzzer corpus, and ASan/UBSan.

**Market microstructure angle** (best for quant research / quant dev hybrid roles):
> Modeled passive order execution with queue-position tracking and cancellation-aware erosion logic; correctly conditions fills on cumulative executed quantity rather than naive price-level trade presence.

**Performance engineering angle** (best for roles emphasizing profiling discipline):
> Drove flamegraph-guided optimization across 3+ profiling iterations using perf hardware counters; documented IPC, L1d miss rate, and branch misprediction deltas per change with full hardware/compiler reproducibility metadata.

---

## Role-Targeting Matrix

| Role                          | Bullet 1 Variant         | Bullet 2 Variant              |
| ----------------------------- | ------------------------ | ----------------------------- |
| HFT quant dev / SWE           | Throughput / systems     | Data structures / algorithms  |
| Low-latency infrastructure    | Microarchitecture        | Performance engineering       |
| Exchange / market data infra  | Protocol / correctness   | Data structures / algorithms  |
| Quant researcher (tech-heavy) | Throughput / systems     | Market microstructure         |
| Trading systems generalist    | Throughput / systems     | Market microstructure         |

---

## Usage Notes

- **Word counts**: each bullet is calibrated to 28–32 words. Do not pad.
- **Numbers are load-bearing**: the metrics (5M events/sec, p99 < 400ns, 11GB, <2% L1 miss) are what make this entry credible. Only cite numbers you have actually measured and can reproduce. Replace TBD entries in README.md before using this on your resume.
- **Technology line**: list only what you directly wrote code against. Do not list Python if your Python contribution is only the mock data generator for a quant dev role — it dilutes the signal. Do list it for broader SWE roles.
- **Project name**: the current name is accurate and specific. Do not shorten to "Order Book Engine" — "NASDAQ ITCH 5.0" is the signal that distinguishes this from a toy matching engine.
- **Tense**: use past tense for completed project, present tense while actively developing. Switch to past before submitting applications.

---

## What Interviewers Will Ask

Anticipate and prepare for these questions, which this project directly surfaces:

1. *How does your price ladder achieve O(1) access?* → Fixed-range tick array, direct index by price_tick.
2. *What happens when an order is in the slab free list vs. live?* → Dual-use next/prev pointers; lifecycle invariant.
3. *Why Robin Hood hashing over standard linear probing?* → Bounds worst-case probe length; better cache behavior at high load factor.
4. *How do you know your replay is deterministic?* → Checksums of order book state snapshots; identical across runs.
5. *What does your fill model get right that naive models miss?* → Queue position erosion from cancellations; fill conditioned on cumulative executed qty ≥ queue_position + size.
6. *What was your biggest performance bottleneck and how did you find it?* → Answer with a specific flamegraph finding from Phase 4.
7. *Why fixed-point prices instead of float?* → Exact representation; no rounding drift across millions of operations; enforced at parser boundary.
