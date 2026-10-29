# Consolidated Book

A C++20 service that connects to the public WebSocket market-data feeds of **Coinbase
Advanced Trade, Kraken WS v2, OKX v5 and Bybit v5** and does the following:

- Maintains a checksum-/sequence-validated L2 order book per venue for BTC and ETH.
- Merges the books into one **consolidated cross-venue book**, converting USDT prices to
  USD at a live USDT/USD rate.
- Computes signals: best cross-venue bid/ask and spread, microprice, top-N imbalance, and
  per-venue staleness.
- Serves a live dashboard with per-stage latency histograms.

Every number in this README was produced by a command shown next to it, from **replay of
recorded live data** that is committed to the repo.

```
 Coinbase I/O   Kraken I/O    OKX I/O      Bybit I/O       one thread per venue: TLS WebSocket (Boost.Beast),
 recv→parse     recv→parse    recv→parse   recv→parse      simdjson On-Demand, sequence checks, normalize
     │ SPSC         │ SPSC        │ SPSC        │ SPSC     lock-free rings of 64-byte events, one batch per message
     ▼              ▼             ▼             ▼
 ┌───────────────────────────────────────────────────────┐
 │ book/signal thread (pinned on Linux, QoS on macOS)     │  venue books (flat sorted arrays, fixed-point int64)
 │  apply → checksum/crossed/horizon checks → consolidate │  → Kraken CRC32 / crossed / horizon checks
 │  → signals → TripleBuffer::publish                     │  → k-way merge + USDT→USD → signals
 └──────────────────────────┬────────────────────────────┘
                            │ wait-free triple buffer (latest value wins)
                            ▼
 ┌───────────────────────────────────────────────────────┐
 │ publisher thread: HTTP + WebSocket server, 20 Hz JSON  │ ──► browsers (via Caddy HTTPS)
 └───────────────────────────────────────────────────────┘
 integrity failure → venue STALE → excluded → reconnect with backoff + jitter → fresh snapshot
```

Design rationale is in [docs/DESIGN.md](docs/DESIGN.md) (including §13, *where
measurements changed the plan*). Interview-style Q&A for every design decision is in
[docs/INTERVIEW_NOTES.md](docs/INTERVIEW_NOTES.md). The Oracle Cloud deployment guide is
[docs/DEPLOY.md](docs/DEPLOY.md).

## Correctness: what is verified, and how

| Claim | Evidence (command) |
|---|---|
| Each venue's protocol was implemented from its **current** docs, then checked live | `docs/probes/` scripts + output from 2026-10-29: 1,755/1,755 Kraken checksums with zlib CRC32; OKX `checksum` field always `0` (deprecated 2026-06-23); Bybit `u` contiguous in 2,036/2,036 deltas |
| C++ books equal an **independent** Python/`Decimal` implementation | `ctest --preset release -R Golden`: 40 s, 4-venue live recording, byte-identical final books (349 lines) |
| Kraken checksum validated on every message | Golden session 5,714/5,714; 4-minute session 65,499/65,499 (`cbook replay`, below) |
| Same results through the real thread layout | `Golden.PipelinedReplayIsDeterministic` (also run under TSan) |
| Gap, checksum, crossed-book, horizon, parse-error and disconnect paths | `tests/unit/test_protocols.cpp`, `test_kraken.cpp` (e.g. OKX broken chain: the message's levels are **never** applied) |
| Memory orders are right, not just "tests pass" | TSan run of the ring test is clean; with `release` deliberately weakened to `relaxed`, TSan reports the race (DESIGN §13, INTERVIEW_NOTES Phase 1) |
| **Zero heap allocations** after warm-up on the hot path | `build/release/tests/cb_alloc_test`: 0 allocations over ~7,500 real frames (control test proves the hook counts) |
| No data races in the running service | Live run of the TSan build with all four venues and a dashboard client: 0 TSan reports |
| Builds and runs on Linux arm64 with GCC | `docker build --platform linux/arm64 -f deploy/Dockerfile .`: GCC 13.3.0, 57/57 tests in the build stage. In the container, golden replay matches and live mode connects all four venues with the book thread **pinned** (`pinned=1`) |

Test counts (2026-10-29, Apple M1): `ctest --preset release` 57/57, `ctest --preset asan-ubsan`
55/55, `ctest --preset tsan` 55/55. The two allocation tests are excluded under sanitizers,
because sanitizers replace `operator new` themselves.

## Measured results

**Hardware:** MacBook Air, Apple M1 (4 performance + 4 efficiency cores), 8 GB RAM,
macOS 26.6.2, Apple clang 21.0.0, `-O3` (CMake Release), on AC power, low-power mode off.
Threads are not pinned (macOS has no hard affinity), and the book/benchmark threads run at
`QOS_CLASS_USER_INTERACTIVE`.

**Clock:** arm64 generic timer, 24 MHz, so **one tick = 41.67 ns**. Any value below ~100 ns
in a percentile column is just a few ticks. For short stages the **mean** column
(total ticks ÷ count, over tens of thousands of messages) is the meaningful number; the
percentiles are quantized.

### Replay of the committed golden session (reproducible from the repo)

```sh
for i in 1 2 3 4 5; do ./build/release/cbook replay --file tests/data/golden_session.cbrec.gz; done
```

40 s of live data from all four venues: 9,970 frames, 10.7 MB. Throughput across the 5 runs
was 286k (cold first run), 421k, 422k, 423k and 427k frames/s. The **median run** (5) is
shown below; all 5 outputs are in `bench/results/2026-10-29_m1/replay_golden_inline_run*.txt`.

```
throughput 421845 frames/s  452.6 MB/s  417995 book-messages/s  2570126 events/s
stage                count       mean       p50       p99     p99.9        max   (ns)
parse/coinbase        1213     7141.8      1417      8958     18958    4484542
parse/kraken          5758      221.7       208       333       667       6333
parse/okx              751     1294.9       792      8792     35958      38375
parse/bybit           2246      391.4       333      1708      3708      10000
queue                 9879       74.8        42       833      3458       6250
book                  9879      454.8       333      3125     10958     164458
signal                9879      259.7       250       542       667       5708
publish               9879      313.6       333       375       417       2292
e2e                   9879     2293.2      1167      8458     25292    4653042
```

The Google Benchmark version of the same pipeline (fresh pipeline per iteration,
construction excluded) agrees:

```sh
./build/release/bench/bench_pipeline_replay --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
# BM_ReplayInline median 23.3 ms per 40 s session, 428.71k frames/s, CV 0.10%
```

### Larger session: 3 min 56 s, 95,106 frames (`bench/data/`, 6.2 MB gzip)

```sh
./build/release/cbook replay --file bench/data/bench_session_2026-10-29.cbrec.gz              # inline, x5
./build/release/cbook replay --pipelined --file bench/data/bench_session_2026-10-29.cbrec.gz  # saturated, x3
./build/release/cbook replay --speed 10 --file bench/data/bench_session_2026-10-29.cbrec.gz   # paced, x3
```

| Mode | What it measures | Result (median run) |
|---|---|---|
| inline (1 thread) | per-stage CPU cost, deterministic | **566,068 frames/s** (runs: 503k, 562k, 566k, 567k, 571k) |
| pipelined, as fast as possible | **saturated throughput** with parse and book on separate cores | **905,812 frames/s** (904.9k, 905.8k, 908.3k). Queue/e2e times here are *backlog*, not latency (Little's law), so they are not reported |
| pipelined, paced at 10× recorded speed (~4,060 frames/s) | **cross-thread latency** at a realistic load, spinning book thread | table below (run 2 of 3, the median e2e p99) |

```
stage                count       mean       p50       p99     p99.9        max   (ns)
parse/coinbase        7723     3846.6      1958     13292     23292    4622667
parse/kraken         65783      348.1       208       583      2542    6163583
parse/okx             4620     1940.8      1250     10292     20958      39708
parse/bybit          16965      561.1       375      2583      8292     222583
queue                94554      871.1       125     10958     54625    2886833
book                 94554      532.3       333      4125     11292    1219708
signal               94554      296.8       250       708      1792     372917
publish              94554      343.4       333       542      1667      77792
e2e                  94554     2793.5      1333     16958     65292    6207333
```

How to read these:
- **Queue p50 = 125 ns = 3 ticks.** That's the cross-core handoff through the SPSC ring. The
  long tail (p99 ~11 µs, max in ms) comes from macOS scheduling unpinned threads: the max
  varied 0.18–11 ms across the 3 runs. Pinned on Linux, that tail is the number to watch.
- **Parse cost scales with the message.** Coinbase's max is the 4.5 MB full-book snapshot
  (~4.5 ms to parse, at session start and on resync). Kraken messages are small and parse
  in ~210 ns.
- **Book (~450–530 ns mean)** is dominated by Kraken's per-message checksum (216 ns,
  below).

### Microbenchmarks (Google Benchmark, medians of 3–5 repetitions)

JSON results are in `bench/results/2026-10-29_m1/`.

| Benchmark | Command | Result |
|---|---|---|
| SPSC push+pop, one core | `bench_spsc` | 7.9 ns (`uint64`), 9.9 ns (64-byte event) |
| SPSC cross-core round trip | `bench_spsc` | 122 ns (≈ 61 ns one way) |
| SPSC cross-core throughput, 64-byte events | `bench_spsc` | 25.6 M/s publishing per item → 84.8 / 95.3 / **400.6 M/s** batching 4 / 16 / 64 |
| Fixed-point decimal parse | `bench_decimal` | 4.9 ns price, 6.4 ns 8-dp qty, 11.3 ns mixed corpus; `strtod`+round baseline 23.3 ns (and inexact) |
| Book modify, 20k-level book | `bench_book` | 18.5–20.3 ns at any rank (binary search) |
| Book insert+delete pair | `bench_book` | rank 0: 31–51 ns · rank 100: 98–131 ns · rank 1,000: ~505–528 ns · rank 19,999: 13.1 µs |
| Kraken checksum, 25-level book | `bench_checksum` | **216 ns** (hardware CRC32 28 ns of it; table CRC32 would take 923 ns) |
| Synthetic parse+apply, Coinbase rank mix | `bench_pipeline_synthetic` | 74.6 ns (binary search) vs 80–82 ns (linear windows 8–64) |

```sh
cmake --preset release && cmake --build --preset release
for b in spsc decimal book checksum pipeline_synthetic pipeline_replay; do
  ./build/release/bench/bench_$b --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
done
```

### Live (not used for any published latency number)
`cbook live` on the M1, 2026-10-29, all four venues: 49.7 MB RSS, 7.4% CPU with the
`backoff` idle policy, 0 book-thread allocations over 75 s. Each I/O thread made 45–66
allocations, all during connect; the counts were identical at 30 s and 55 s.

## What I tried that did not work

1. **A linear scan from the best level before binary search.** It's 2× faster for rank-0
   updates on tiny books but ~7% slower on the measured Coinbase rank mix (median rank 33).
   Removed; it's still available as a template parameter.
2. **Publishing each event to the ring separately.** Measured 16× slower than batching.
   The `tail_` cache line bouncing between cores, not the payload, was the cost.
3. **128-bit division for USDT→USD conversion inside the merge loop.** `__divti3` is a
   library call on arm64, and the signal stage took 1,155 ns. Splitting into 64-bit parts
   (bit-identical, tested on 10⁶ inputs) plus caching brought it to 267 ns.
4. **Naive digit formatting for Kraken's checksum** (434 ns). Compile-time divisors and a
   2-digit lookup table brought it to 216 ns.
5. **Whole-message reads for liveness.** Coinbase's 4.5 MB snapshot took more than 5 s on a
   slow link, and the watchdog killed a healthy connection. Liveness now tracks bytes.
6. **OKX's documented port 8443.** The TCP connection was reset from India; port 443 works.
7. **Validating OKX's checksum.** It was deprecated on 2026-06-23 and is always 0.
   Implementing it from memory would have failed every message; the sequence chain is the
   check.
8. **A dedicated recorder thread with byte rings** (in the plan). Not built: recording runs
   are never used for latency, so a mutex plus an 8 MiB stdio buffer is enough.

## Known limitations

- **Not yet measured on Ampere.** All numbers are from the M1. The deployment guide includes
  recording a session on the VM and replaying it there; CI also runs on arm64 Linux.
- **Timer resolution is ~40 ns** on both M1 (measured) and Ampere (expected ~25 MHz, verify
  with the command in DEPLOY.md). Sub-100 ns percentiles are a few ticks; use the means.
- **"Receive" is user space after TLS decryption**, not a NIC/kernel timestamp. Network
  latency to the exchanges is out of scope, and the exchange-lag column mixes one-way
  latency and clock offset (it's labeled that way).
- **macOS can't hard-pin threads**, so the max/p99.9 tails on the M1 include scheduler noise.
- **Bybit's `u` contiguity is undocumented.** It's treated as a gap rule because 2,036/2,036
  live deltas were contiguous. If Bybit ever skips legitimately, that venue will resync more
  than it needs to.
- **The Coinbase horizon (±2%)** means a move of more than ~2% triggers a resync (one 4.5 MB
  snapshot, a few seconds stale). Inside the band the book is exact.
- **Coinbase and Bybit have no checksum.** Integrity relies on sequence numbers plus the
  crossed-book and horizon checks. The optional "audit resync" in DESIGN §6.5 is not built.
- **One FX source.** If Kraken's USDT/USD book is down, USDT venues leave the consolidated
  book; there's no fallback rate.
- **Venue resync = reconnect.** Simple and always correct, but one bad symbol reconnects the
  venue's other symbols too.
- **Not yet published:** CI has never run (no git remote), and the Oracle VM hasn't been
  created. The Docker image was built and smoke-tested locally (Docker Desktop, linux/arm64),
  which covers the Linux/GCC path.

## Running it

**Prerequisites (macOS):** Xcode command line tools, `brew install cmake openssl@3`
(optional: `ninja`). **Ubuntu 24.04:** `build-essential cmake libssl-dev zlib1g-dev`.
CMake fetches simdjson v5.0.5, Boost 1.92.0 (hash-pinned), GoogleTest v1.18.0 and Google
Benchmark v1.10.0.

```sh
cmake --preset release && cmake --build --preset release   # also: debug, asan-ubsan, tsan
ctest --preset release

./build/release/cbook live                         # dashboard: http://127.0.0.1:8080/
./build/release/cbook live --record my.cbrec       # also record raw frames
./build/release/cbook replay --file my.cbrec       # deterministic, prints stage table
./build/release/cbook replay --file my.cbrec --speed 10   # threaded, paced: latency
./build/release/cbook replay --file my.cbrec --dump books.txt [--dump-consolidated]
python3 -I tools/golden_reference.py my.cbrec      # independent book reconstruction
```

Configuration lives in `config/default.json` (local) and `config/production.json` (VM): per-venue
`enabled`/`url`/`depth`, Coinbase `horizon_bp`/`min_levels`, `book_cpu`, `idle`
(`spin`|`backoff`), and the HTTP bind address, port and web root. Any venue can be disabled
without affecting the others.

**Deploy:** `docker build -f deploy/Dockerfile -t cbook .`, then follow
[docs/DEPLOY.md](docs/DEPLOY.md) (Oracle Cloud A1, firewall, Caddy HTTPS, systemd
alternative).

## Repository layout

```
src/core       fixed-point + decimal parser, SPSC ring, triple buffer, histogram, CRC32, clock
src/book       BookSide / OrderBook (flat sorted arrays, best level at the back)
src/venue      Event, EventSink, protocol base + coinbase / kraken / okx / bybit, checksum
src/engine     Engine (books, checks, consolidated, signals), Pipeline wiring, config
src/record     .cbrec format, recorder, reader, inline / pipelined / paced replay
src/net        Beast TLS WebSocket session (connect, subscribe, ping, liveness, backoff)
src/publish    dashboard server + JSON
src/app        cbook main, per-thread allocation counting hooks
web/           index.html + app.js (vanilla JS, canvas)
tests/         unit, golden (vs tools/golden_reference.py), allocation test, live fixtures
bench/         Google Benchmark suites, recorded session, results
deploy/        Dockerfile, systemd unit, Caddyfile        .github/workflows/ci.yml
docs/          DESIGN.md, INTERVIEW_NOTES.md, DEPLOY.md, probes/
```
