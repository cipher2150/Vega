# Interview Notes

Each phase adds its key decisions, the questions an interviewer is likely to ask about
them, and short model answers. **Say these in your own words.** If you can't rebuild
an answer from first principles, reread the code until you can.

---

## Phase 0 — Plan

### Key decisions (details in DESIGN.md)
1. Thread layout: one I/O thread per venue → SPSC queue per venue → one book/signal thread → triple buffer → publisher thread.
2. Prices and sizes are int64 at a universal 1e-8 scale. Products use `__int128`.
3. The book is a flat sorted array with the best price at the back. Full-depth Coinbase gets a price horizon.
4. Resync = reconnect, for every venue, with exponential backoff and jitter.
5. OKX's checksum is deprecated, so we rely on the seqId chain. Kraken's CRC32 is our strongest correctness check, which is why Kraken is the first venue.
6. Measurement: the arm64 generic timer (24 MHz on M1). Sub-tick stages are reported as batch averages.
7. One library (Boost.Beast) for both the feed client and the dashboard server.

### Likely questions and model answers

**Q: Why SPSC queues and not one MPMC queue?**
- Every queue in this design really has one producer (the venue's I/O thread) and one consumer (the book thread), so we don't pay for generality we don't use.
- An MPMC queue needs a CAS on head or tail, because several threads race for the same index. Under contention, CAS retries and cache-line ping-pong between producers make latency unpredictable.
- In an SPSC queue, each index has exactly one writer, so plain `store(release)` is enough: no read-modify-write, no retry loop.
- Per-venue queues also isolate venues: a burst from Coinbase can't delay Kraken events stuck behind it in a shared FIFO, and the book thread decides the polling order.

**Q: Justify each memory order in your ring buffer.**
- Producer: write slot, then `tail.store(release)`. Release means nothing written before the store (the slot data) can become visible after it. A consumer that sees the new tail is guaranteed to see the data.
- Consumer: `tail.load(acquire)` is the matching acquire. It creates a happens-before edge from the producer's slot write to the consumer's slot read.
- Consumer: read slot, then `head.store(release)`. The release here protects a *read*: it guarantees the consumer has finished reading the slot before the producer can learn the slot is free and overwrite it. Without it, the store could become visible before the read completes, and the producer could overwrite data mid-read.
- Producer: `head.load(acquire)` pairs with that release.
- Each thread loads its own index `relaxed`: only that thread writes it, so it always sees its own latest value.
- No `seq_cst`: we only need pairwise happens-before between two threads, not a single global order across all atomics. On ARM, acquire/release compile to `ldar`/`stlr` (or `ldapr`), which are cheaper than the `dmb` fences seq_cst may need.

**Q: Why fixed-point instead of double?**
- **Exact equality:** a book is a map from price to quantity. The same price from two messages must compare equal, or an update creates a duplicate level instead of replacing one. Decimal strings like `0.1` have no exact binary64 representation.
- **Checksums:** Kraken's CRC32 is computed over the decimal digits. We have to regenerate exactly the exchange's digits, which is trivial from an integer and fragile from a double.
- **Determinism:** integer math gives bit-identical results across compilers, platforms, and replays. That's what lets golden-file tests compare final books exactly.
- **Speed:** parsing digits straight into an integer is faster than a correct `strtod`.
- Why 1e-8: it covers every BTC/ETH tick and lot on these four venues. int64 still allows prices up to 9.2×10¹⁰. price×qty needs `__int128`.

**Q: How did you avoid false sharing?**
- The producer-written index (`tail`) and the consumer-written index (`head`) sit on separate cache lines. Each side's cached copy of the other index sits on its own line too. Without this, every push would invalidate the line the consumer is spinning on, and vice versa.
- The line size is a compile-time constant per platform: 128 on Apple M1 (`sysctl hw.cachelinesize` says 128), 64 on Ampere Altra (Neoverse N1).
- I don't use `std::hardware_destructive_interference_size`: GCC on aarch64 reports 256 and warns that the value isn't ABI-stable.
- A test checks the runtime line size against the constant.

**Q: Why does each side cache the other's index?**
- Reading the other thread's index is a cross-core cache miss, often the single most expensive operation in the queue.
- The producer only needs `head` when it *thinks* the queue is full, and the consumer only needs `tail` when it thinks the queue is empty.
- So in steady state each thread touches only its own cache lines.

**Q: How does resync work?**
- Any integrity failure moves the venue to STALE and removes it from the consolidated book immediately: a sequence gap, a checksum mismatch, a crossed book, a parse error, a full queue, a liveness timeout, or a socket error.
- Detection happens where the information lives. Sequence checks run on the I/O thread, which owns the protocol state. Checksum and crossed-book checks run on the book thread, which owns the book.
- The book thread tells the I/O thread to resync by bumping a per-venue atomic `resync_epoch`. Every event carries the epoch it was produced under, and the book thread drops events from older epochs. In-flight data from the dead session therefore can't corrupt the new book.
- The I/O thread closes the connection and reconnects after exponential backoff (250 ms × 2^k, capped at 30 s, ±20 % jitter, reset after 60 s healthy).
- Then it resubscribes. All four venues send a fresh snapshot on subscribe, so there's no "fetch REST snapshot and splice buffered deltas" dance. That dance is what Binance needs, and it's where most real bugs live.

**Q: Why reconnect instead of just resubscribing?**
- It's one mechanism that's correct for every venue.
- Coinbase's sequence number is per connection, so a gap there invalidates the whole connection anyway.
- It stays well inside the rate limits (OKX: 3 connections/s per IP; Bybit: 500 per 5 min).
- Per-symbol resubscribe would shrink the blast radius, but it's an optimization I'd add only if measurements justify it.

**Q: Coinbase sends ~20,000 levels per side. Why not a price-indexed ladder?**
- I measured first. In 60 s of live BTC-USD, 85 % of level updates were within 100 levels of the top, and 98 % within 1,000.
- With a sorted array whose best level is at the back, an insert or delete moves only the levels between it and the top, so the typical `memmove` is a few hundred bytes.
- I bound the worst case with a price horizon: at snapshot time I keep only ±2 % around mid and ignore updates outside it. Inside the band the book is *exact*, because every insert and delete in that price range is applied. If the market moves so the band runs low on levels, I resync with a new band.
- A ladder would give O(1) updates, but costs ~2.5 MB per side at a 0.01 tick, needs a bitmap scan to find the next best price after deleting the best, and needs a recentering scheme.
- Phase 1 benchmarks update cost at depths 0/10/100/1000 to confirm the choice.

**Q: Why is it safe to ignore Coinbase updates outside your horizon?**
- Correctness only matters inside the price range we serve. Each update carries an absolute new quantity for a specific price, so an update outside the band can't affect any price inside it.
- The book is exact within the band. The only risk is the market *moving* to the band edge, and that triggers a resync.

**Q: What limits your measurement accuracy?**
1. **Timer resolution:** the arm64 generic counter runs at 24 MHz on M1 (I read `cntfrq_el0`), so one tick is ≈ 41.7 ns. Ampere is about the same, ~25 MHz. A 15 ns book update shows up as 0 or 1 tick. For those stages I report the batch average (total ticks ÷ N over a million events) rather than claiming precise percentiles.
2. **What "receive" means:** it's when the user-space read completes, *after* the kernel, TLS decryption, and WebSocket deframing. Kernel and NIC time are invisible to us.
3. **The probe effect:** each timestamp read costs a few ns plus an `isb` barrier, which perturbs what it measures.
4. **Cross-thread stages:** `parse → dequeue` subtracts timestamps from two cores. That's valid because the generic timer is a single system-wide counter, unlike x86 TSC, which needs an invariant-TSC check.
5. **The environment:** the laptop has no thread pinning (QoS is only a hint), there's DVFS on both machines, and the VM has a noisy-neighbour hypervisor. That's why published numbers come from deterministic replay with the hardware, OS, and command stated.
6. **Exchange clock offset:** `local_recv − exchange_ts` equals one-way latency plus clock offset, and the two can't be separated without a round trip. I show the 60 s minimum (latency floor plus offset) and, for Kraken, an NTP-style estimate from ping `time_in`/`time_out` with its error bound.

**Q: The OKX checksum: did you validate it?**
- No, and that was deliberate. OKX deprecated the book checksum on 2026-06-23. The field is still sent but always reads `0`, which I confirmed live.
- The official replacement is the `seqId`/`prevSeqId` chain: each update's `prevSeqId` must equal the previous message's `seqId`. The snapshot has `prevSeqId = -1`.
- Lesson: read the current docs and changelog. Coding a protocol from memory would have produced a validator that fails on every message.

**Q: How do you detect a gap on Kraken without sequence numbers?**
- With the checksum: a CRC32 of the top 10 levels arrives with every update. If we miss a message, our book differs, and the next checksum mismatches with probability ≈ 1 − 2⁻³².
- The one thing it can't catch is a missed change *below* the top 10 that never moves into the top 10 before it's overwritten. That's harmless for anything we compute from the top of book.

**Q: Why convert USDT to USD instead of treating them as equal?**
- USDT is a different asset whose price drifts a few basis points around $1. Treating it as equal would show a fake cross-venue spread (Coinbase USD vs OKX USDT) that's really USDT basis, not arbitrage.
- I convert with the live Kraken USDT/USD mid. If that rate goes stale, the USDT venues drop out of the consolidated book rather than using a guess.

**Q: Why Boost.Beast instead of uWebSockets?**
- The hard part is the *client* side: four TLS WebSocket feeds. uWS is a server library, so I'd need a second library anyway.
- Beast covers both sides with one event-loop model, and it lets me own the buffers: a reused `flat_buffer` sized up front means no per-message allocation.
- The dashboard server sends ~20 messages/s to a few browsers, so uWS's raw speed doesn't matter.
- Beast's cost is compile time and memory, which I contain to two translation units.

**Q: Is your "zero allocations" claim really true end to end?**
- It's precise about its scope: the test overrides global `operator new`/`delete`, warms up on part of a replay, then asserts zero allocations from parse through the publish hand-off.
- `operator new` doesn't see `malloc` inside C libraries like OpenSSL. So the live network path gets a separate measurement with `malloc` interposition, and I report that number as measured, whatever it is.

---

## Phase 1 — Core: fixed-point, SPSC ring, order book

### Key decisions
- `parse_decimal` is strict: it accepts only `digits[.digits]` and rejects signs, exponents, whitespace, a non-zero 9th decimal, and overflow. Every rejection is an error, never a guess.
- The ring uses monotonic `uint64_t` indices, `alignas(kCacheLine)` on each of four fields, a cached copy of the opposite index on each side, and batch push/consume.
- Book sides are structure-of-arrays with the best level at the back. Asks store the *negated* price, so both sides sort ascending and share one code path.

### Questions
**Q: Why does `parse_decimal` reject "1e5" instead of handling it?**
No venue sends exponents for prices or sizes. If one ever does, the protocol has changed and I want to find out (the venue goes stale and resyncs), not keep running on a value I may have misread. The same goes for a 9th decimal: rounding would silently desync the book from the exchange.

**Q: How do you know your memory orders are right, not just "tests pass"?**
I ran a negative control. In a scratch copy I weakened the producer's `tail_.store(release)` to `relaxed`, and TSan reported a data race between `try_push` writing the slot and `try_pop` reading it. The correct code is clean under TSan. A plain stress test can pass on ARM even with the bug, because the hardware happened not to reorder in that run. TSan checks the C++ memory model itself, which makes it a stronger check than any amount of stress testing.

**Q: Why store the negated price for asks?**
"Better" is then always "larger key" on both sides, so search, insert and erase exist once instead of twice, and there's half as much code to get wrong. Prices are positive, so negation can't overflow.

**Q: What did the ring benchmarks show?**
- Push+pop on one core: 7.9 ns.
- Cross-core round trip: 122 ns (≈ 61 ns one way).
- Cross-core throughput: 25.6 M items/s publishing each item, 400.6 M items/s publishing batches of 64.

The limit is the cache-line handoff, not the copy (64-byte items weren't slower than 8-byte ones), so the design publishes once per venue message.

**Q: Why binary search when you said the hot levels are near the top?**
I measured the alternative first. A linear scan from the best end wins only at rank ≈ 0 (2.3 ns vs 4.6 ns). Coinbase's median update rank is 33, past any small window, and on the realistic rank mix pure binary search was ~7% faster. It's a template parameter, so replay data can revisit the choice.

**Q: Why is a deep insert slow, and why is that acceptable?**
Insert/delete shifts every level between the touched one and the best one. At rank 1,000 a pair costs ≈ 530 ns, and at rank 20,000 it's 13 µs. The horizon (±2%, at most ~1,800 levels/side for BTC) bounds the worst case, and live data puts 98% of updates within rank 1,000. If the tail still mattered, the answer is a price-indexed ladder behind the same interface.

## Phase 2 — First venue (Kraken), record/replay

**Q: Why Kraken first?**
It's the only venue with a working per-message checksum (CRC32 of the top 10 levels), so every book state of the first end-to-end venue is verified by the exchange itself. The fixture test replays 300 live frames with zero mismatches. The full golden session validated all 5,714 checksums, and the 75 s session all 18,590.

**Q: Kraken has no sequence numbers. How do you detect a gap?**
Through the checksum: a missed update makes our top 10 differ, so the next checksum fails (probability ≈ 1 − 2⁻³²). The blind spot is a missed change *below* the top 10 that is overwritten before it ever reaches the top 10, and that can't affect anything computed from the top of the book.

**Q: Your checksum code formats prices back from integers. Isn't keeping the original string simpler?**
Keeping strings would mean either allocating or storing variable-length text in the book. At the instrument's precision (from Kraken's `instrument` channel: BTC/USD 1/8, ETH/USD 2/8, USDT/USD 5/8), the digits are exactly `value / 10^(8−precision)`, so the book stays two int64 arrays. Cost: 221 ns per message after optimization (434 ns before), with the hardware CRC32 instruction at 28 ns of that.

**Q: How does replay stay deterministic?**
- Frames are recorded after TLS and WebSocket deframing, along with connect/disconnect markers and session epochs.
- Replay feeds them to the same protocol objects with the recorded wall time, so exchange-lag statistics replay identically. It uses the current monotonic clock for latency, so it measures our code, not the original network.
- Shutdown disconnects are tagged and skipped, so a recording's end isn't treated as a market event.

**Q: What does "resync" do concretely?**
1. Whoever detects the problem marks it. The I/O thread does this for sequence gaps and parse errors by emitting `VenueDown`. The book thread does it for checksum failures, crossed books, a full book or an exhausted horizon, by writing `resync_request = epoch` with a release store.
2. The engine marks every book of that venue stale and sets `min_epoch = epoch + 1`, so any events still in flight are dropped.
3. The I/O watchdog sees the request (acquire load) and closes the socket. The session loop backs off (250 ms·2^k, ±20% jitter, capped at 30 s, reset after 60 s healthy), reconnects, and resubscribes, and each venue then sends a fresh snapshot.

## Phase 3 — All venues, consolidated book, signals

**Q: OKX and Bybit put the sequence number *after* the levels. How do you avoid applying a bad message?**
Events go into a staging buffer (`EventSink`) and are pushed to the ring only when the whole message has validated. On a broken chain the staging buffer is discarded, and the book thread never sees those levels. The test `Okx.BrokenChainDiscardsTheMessageAndGoesStale` asserts it.

**Q: Coinbase's sequence number is per connection. Why does that matter?**
Heartbeats share the counter. Subscribe to heartbeats, then check only the l2_data messages, and every heartbeat looks like a gap. Conversely, a dropped heartbeat *is* a gap on the connection. So the check runs on every message, before looking at the channel.

**Q: How is the consolidated book built?**
A k-way merge of each live venue's top 20 per side (they're already sorted), with USDT prices converted to USD. It's O(rows × venues), allocation-free, and ties break by venue index for determinism. At 4 venues × 20 levels a full recompute is cheaper than maintaining it incrementally, and much simpler.

**Q: What's the hardest correctness bug you hit?**
Two came from measurement:
- **Liveness tracked frames instead of bytes.** Coinbase's 4.5 MB snapshot took more than 5 s on a slow link, so the watchdog killed a healthy connection. It now refreshes liveness on every partial read.
- **Shutdown made every book stale in replay.** That broke golden comparisons, so disconnect records now carry their reason and shutdowns are skipped.

**Q: How do you know the books are right, beyond unit tests?**
Golden tests compare against a *separate* Python/`Decimal` implementation of the same rules (`tools/golden_reference.py`). On a 40 s, four-venue live recording the two produce byte-identical books (349 lines).

## Phase 4 — Instrumentation

**Q: Explain your histogram.**
It's log-linear like HdrHistogram: values below 32 get exact buckets, and each power of two above that is split into 32 sub-buckets, so any value is known to within ~3%. There's one writer per histogram, and counters are atomics updated with relaxed load+store (not `fetch_add`, which with a single writer would only add cost). Readers take a possibly torn snapshot, which is fine for display.

**Q: What does "zero allocations" cover exactly?**
`cb_alloc_test` replaces global `operator new` and replays the golden session: 25% warm-up, then the remaining ~7,500 frames through parse → staging → ring → book → checks → consolidated → signals → triple buffer, with zero allocations. A control test proves the hook counts. Live, each I/O thread and the book thread count their own allocations; the book thread stays at 0, while the I/O threads show a few dozen at connect time (TLS, coroutine frames).

**Q: Why is the live queue latency p99 64 µs but replay's 1 µs?**
That's the `backoff` idle policy: the book thread sleeps 50 µs when idle, so the first message after a quiet spell waits up to that long. `spin` removes it at the price of a full core. The trade-off was measured, not assumed.

## Phase 5 — Dashboard

**Q: How can a slow browser not slow the book thread?**
There are two decoupling points:
- **Book thread → publisher:** a wait-free triple buffer. The book thread never waits; the publisher takes the newest snapshot at 20 Hz.
- **Publisher → each browser:** a single pending slot per message kind. While a write is in flight, a newer snapshot replaces the pending one, so memory per client is bounded and a slow client only skips frames.

**Q: Why JSON and not binary?**
At 20 Hz × a few KB it's ~100 KB/s per client, and it's built on the publisher thread, off the hot path. JSON keeps the page dependency-free and debuggable. Prices are emitted as exact decimal text from fixed-point, so even display values are never rounded through `double` on the server.

## Phase 6 — Deployment

**Q: Why does Oracle's Ubuntu VM not answer on port 443 even with the security list open?**
The image ships its own iptables rules that REJECT everything but SSH. Both layers must be opened (docs/DEPLOY.md §2).

**Q: Why Caddy?**
Automatic Let's Encrypt certificates and transparent WebSocket proxying in a few lines of config. The service itself listens only on localhost.

## Phase 7 — Results and honesty

**Q: What's your headline number, and how would I reproduce it?**
`cbook replay --file tests/data/golden_session.cbrec.gz` gives ≈ 420k frames/s single-threaded on an M1: the median of 5 runs, with the first (cold) run discarded and reported separately. The pipelined mode reaches ≈ 906k frames/s on the 4-minute session. The recordings are in the repo, so the input is identical for everyone; only the hardware differs.

**Q: Why are there three replay modes?**
They answer three different questions:
- **Inline** gives deterministic per-stage CPU cost.
- **Pipelined as-fast-as-possible** gives saturated throughput. Its queue times are backlog (Little's law), so I deliberately don't report them as latency.
- **Paced (`--speed 10`)** gives cross-thread latency at a realistic load: queue p50 = 125 ns (3 ticks of the 24 MHz timer).

Mixing them up is the classic way benchmark numbers mislead.

**Q: What would you do next?**
1. Record and replay on the Ampere VM, where pinning works, so the tails measure the code, not the macOS scheduler.
2. Add the audit resync for venues without a checksum (Coinbase, Bybit).
3. Make resync per symbol instead of per connection.
4. If Coinbase's deep-insert tail ever matters, put a price-indexed ladder behind the same `BookSide` interface.
5. Use kernel receive timestamps (`SO_TIMESTAMPING`) to extend measurement below TLS. Each TLS record can carry several WebSocket frames, though, which limits how precise that can be.
