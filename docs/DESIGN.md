# Consolidated Book — Design

Status: **implemented (Phases 0–7)**, 2026-10-29. Where the implementation departed from the
plan because a measurement said so, the plan text is kept and §13 explains the change. Every protocol claim below either cites the
official doc it came from or is marked **[LIVE]** (observed by the probe in
`docs/probes/`, run from an Indian IP on 2026-10-29) or **[VERIFY]** (could not
confirm from official docs; see §9).

---

## 1. Goals and non-goals

**Goals**
- Correct L2 books for 4 venues × 2 instruments, with automatic detection and recovery
  of anything that makes a book untrustworthy.
- A consolidated cross-venue book and a few signals, served live to a browser.
- Honest, reproducible latency numbers for each stage, all produced from replay.

**Non-goals**
- Order entry, fees, or trading logic. Signals are descriptive only.
- Wire-level latency (NIC timestamps, kernel bypass). The feeds arrive over TLS on the
  public internet, so internet jitter (milliseconds) dwarfs anything we could gain below
  the socket API. The latency we measure is ours: user space, after TLS decrypt.

## 2. Decisions that change the original spec

These come straight from what the current docs and live probes showed.

| # | Spec said | Reality | What we do |
|---|---|---|---|
| D1 | Validate checksums where provided | **OKX deprecated its book checksum on 2026-06-23.** The field is still sent but is always `0` ([OKX changelog](https://www.okx.com/docs-v5/log_en/); [LIVE] `"checksum":0`). | OKX integrity = `seqId`/`prevSeqId` chain only. |
| D2 | — | **Coinbase and Bybit publish no checksum.** | Sequence/update-ID checks plus a crossed-book check. An optional periodic "audit resync" (§6.4) compares our book against a fresh snapshot. |
| D3 | BTC-USD / BTC-USDT consolidated | USD and USDT are different assets, and the basis is usually a few bp. | USDT books are converted to USD with a **live Kraken `USDT/USD` mid** (decided 2026-10-29). Raw venue books keep native quotes. |
| D4 | Fixed-point in ticks/lots *per instrument* | Per-venue ticks differ (Coinbase BTC 0.01, Kraken/OKX 0.1), and USDT→USD conversion produces off-tick prices. | **One universal scale: price and qty are both int64 × 10⁻⁸.** Cross-venue comparisons need no rescaling. Products use `__int128`. See §5.1. |
| D5 | Flat array or ladder | Coinbase sends the **entire book**: [LIVE] ~20k levels/side in a 4.5 MB snapshot frame. | A flat sorted array with a **price horizon** for full-depth venues (§5.2). The data justifying this is in §5.2. |
| D6 | M1 timer is 24 MHz | Confirmed: `cntfrq_el0 = 24000000`. Ampere Altra's generic timer is also ~25 MHz [VERIFY on VM]. | The ~40 ns resolution limit **applies in production too**, not just on the Mac. Stages shorter than one tick are reported as batch averages (§7). |
| D7 | uWebSockets or Beast | We also need a TLS WebSocket **client**, which uWS doesn't provide in usable form. | **Boost.Beast** for both client and server: one dependency and one event-loop model. See §8. |
| D8 | HdrHistogram_c or own | — | Our own log-linear histogram: single writer, no allocation, readable concurrently via relaxed atomics (§7). |
| D9 | OKX on `wss://ws.okx.com:8443` | [LIVE] Port 8443 got TCP-reset from India; port 443 worked. | URL comes from config, defaulting to `wss://ws.okx.com/ws/v5/public` (443). |

## 3. Architecture

```
                    one I/O thread per venue (Boost.Asio io_context, TLS, Beast WS)
 ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
 │ Coinbase I/O │  │  Kraken I/O  │  │   OKX I/O    │  │  Bybit I/O   │
 │ recv frame   │  │ recv frame   │  │ recv frame   │  │ recv frame   │   t_recv
 │ simdjson OD  │  │ simdjson OD  │  │ simdjson OD  │  │ simdjson OD  │   t_parsed
 │ seq check    │  │ (no seq)     │  │ seqId chain  │  │ u check      │
 │ normalize →  │  │ normalize →  │  │ normalize →  │  │ normalize →  │
 └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
        │ SPSC<Event>     │ SPSC<Event>     │ SPSC<Event>     │ SPSC<Event>
        │ (+ raw frames → Recorder SPSC → recorder thread → .cbrec file, optional)
        ▼                 ▼                 ▼                 ▼
 ┌──────────────────────────────────────────────────────────────────────┐
 │ Book/signal thread (pinned on Linux, QoS USER_INTERACTIVE on macOS)  │
 │  poll queues round-robin → apply to VenueBook        t_dequeued      │
 │  per message end: checksum (Kraken), crossed check   t_book          │
 │  update ConsolidatedBook (USDT→USD), signals          t_signal        │
 │  write PublishSnapshot into TripleBuffer             t_published     │
 │  on integrity failure: venue→STALE, bump resync_epoch (atomic) ──────┼──▶ I/O thread
 └──────────────────────────────┬───────────────────────────────────────┘    reconnects
                                │ TripleBuffer<PublishSnapshot> (wait-free, latest-wins)
                                ▼
 ┌──────────────────────────────────────────────────────────────────────┐
 │ Publisher thread (Beast HTTP+WS server, its own io_context)          │
 │  every 50 ms: take latest snapshot + histograms → JSON → all clients │
 │  serves web/index.html; slow clients drop frames, never block.       │
 └──────────────────────────────────────────────────────────────────────┘
                                ▲ HTTPS/WSS via Caddy reverse proxy
                             browsers
```

Replay mode swaps the four I/O threads for a single `ReplaySource` that reads the
`.cbrec` file and feeds the **same** parser/normalizer objects. There are two
replay modes:
- **inline**: one thread runs parse → book → signals. Deterministic, and used for
  golden tests and per-stage costs.
- **pipelined**: the real thread layout, used to measure queue hand-off costs.

## 4. Thread model

| Thread | Count | Owns | Blocks? | Placement |
|---|---|---|---|---|
| Venue I/O | 1 per enabled venue | socket, TLS, WS framing, simdjson parser, protocol state (seq numbers, subscription state, reconnect/backoff) | yes, in `epoll`/`kqueue` (that's fine: its only job is to wait for bytes) | unpinned |
| Book/signal | 1 | all venue books, consolidated book, signals, stage histograms | **never**: spins on queues, with optional backoff | Linux: `pthread_setaffinity_np` to a dedicated core. macOS: `QOS_CLASS_USER_INTERACTIVE` (macOS has no hard pinning API) |
| Publisher | 1 | WS server, client sessions, JSON encoding | yes, in its own io_context | unpinned |
| Recorder | 0 or 1 | output file | yes, in `write` | unpinned |

- **One I/O thread per venue**, not one shared thread. Each SPSC queue then really has a
  single producer, and Coinbase's 4.5 MB snapshot parse (milliseconds) can't stall the
  other venues. The cost is 4 mostly-idle threads, which is cheap on the free-tier VM
  (4 Ampere cores).
- **Parsing happens on the I/O thread.** The four venues parse in parallel, the book
  thread only sees fixed-size, already-normalized events, and the SPSC slot type can
  be a trivially copyable 64-byte struct. Checks that need the book (Kraken checksum,
  crossed book) run on the book thread.
- **Back-channel** (book thread → I/O thread, "please resync"): a per-venue
  `std::atomic<uint32_t> resync_epoch`. The I/O thread checks it between reads. Every
  event is tagged with the epoch it was produced under, and the book thread drops events
  from older epochs. This rules out in-flight events from a dead session ever touching a
  freshly reset book.
- **Book thread idle policy** (config): `spin` (lowest latency, burns a core: the
  production choice) or `backoff` (spin N iterations, then `yield`, then a short sleep:
  the laptop choice). The policy is printed with every benchmark result.

### Platform abstraction

`platform/thread.hpp` has one function, `set_current_thread_role(Role, optional<int> cpu)`.
- **Linux:** `pthread_setaffinity_np` + `pthread_setname_np`.
- **macOS:** `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE)` + name. The cpu
  index is ignored, and a warning is logged once.

### Cache-line constant

`platform/cacheline.hpp`:
`inline constexpr std::size_t kCacheLine = (__APPLE__ && __aarch64__) ? 128 : 64;`

- We don't use `std::hardware_destructive_interference_size`. GCC reports 256 on
  aarch64 and warns (`-Winterference-size`) that the value is ABI-unstable, and libc++
  support has varied by version.
- 128 on M1 matches `sysctl hw.cachelinesize` (verified: 128).
- 64 on Neoverse N1 (Ampere Altra) is its L1/L2 line size.
- A unit test asserts at startup that the runtime value (sysctl on macOS, `sysconf(_SC_LEVEL1_DCACHE_LINESIZE)`
  or `/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size` on Linux) is ≤ the
  compile-time constant.

## 5. Data structures

### 5.1 Fixed-point

```
struct Price { int64_t v; };   // value × 1e-8. ±9.2e10 → room for any crypto price
struct Qty   { int64_t v; };   // value × 1e-8. ±9.2e10 units; BTC/ETH sizes need ≤8 dp
```
- **The parser** goes directly from a decimal string or raw JSON number token
  (`"80006.3"`, `0.00006120`) to int64. It:
  - accumulates the integer digits;
  - accumulates up to 8 fractional digits, then scales by 10^(8 − n);
  - **rejects** a 9th non-zero fractional digit, exponents, signs, empty input, and overflow
    (it returns an error code and never silently rounds). A rejection is treated as a
    protocol error for that venue: it goes stale and resyncs.
  
  No floating point is involved at any step.
- **Why not double:** `0.1 + 0.2 != 0.3`. Equal prices from two messages must compare
  exactly equal, or a level gets duplicated instead of updated. Doubles also make a
  checksum reproduction impossible to guarantee.
- **Why one universal scale, not per-instrument ticks:** the books are sorted arrays, not
  ladders, so they never index by tick (§5.2). A shared scale makes cross-venue merges
  and USDT conversion plain integer operations. Per-instrument `tick` and `lot` are still
  in config. They are used to validate input and to format Kraken's checksum string
  (which needs the instrument's display precision).
- **Products:** microprice, imbalance, and USDT→USD conversion multiply two 1e-8 values,
  which needs up to ~10²⁴, so they use `__int128` (supported by Clang and GCC on
  arm64). Division happens last, with explicit round-half-even.

### 5.2 Per-venue book: flat sorted arrays, best at the back

```
template <size_t Cap>
struct BookSide {                  // one per side
  std::array<int64_t, Cap> px;     // sorted so the BEST price is at index n-1
  std::array<int64_t, Cap> qty;    // structure-of-arrays: searches touch only px
  uint32_t n;
};
```
- **Bids are sorted ascending and asks descending**, so the best level is always at the
  back. Most updates land near the top of book, so an insert or delete only `memmove`s
  the few elements between it and the back.
- **Lookup** was planned as a linear scan of the first K levels from the back, then a
  binary search. Measurement chose pure binary search (K = 0) instead (§13.2).

**Measured access pattern** (Coinbase BTC-USD, 60 s live, 17,879 level updates, rank 0 = best):

| Updates within rank | p50 rank | p90 | p99 | max |
|---|---|---|---|---|
| <100: **85.4 %** · <1000: **97.9 %** | 33 | 113 | 7,911 | 20,068 |

Command: `python3 -I docs/probes/probe_protocols.py 60` (output in `docs/probes/probe_2026-10-29.txt`).

**Price horizon (for full-depth venues, i.e. Coinbase).** At snapshot time, the book
keeps only levels within a configurable band around mid (default ±2 %) and records the
band edges as the **horizon**.
- Updates outside the horizon are dropped at parse time on the I/O thread.
- Inside the horizon, the book stays **exact**: every insert and delete at a price in the
  band is applied, however many levels that adds up to.
- If the market moves so that fewer than `min_levels` (default 50) remain inside the
  horizon on either side, or the band overflows `Cap`, the venue resyncs and a new
  horizon is set.
- This bounds `memmove` to `Cap` elements and memory to a few hundred KB per book.
  Exact top-of-book correctness is unaffected.

**Depth-limited venues** (Kraken 25, OKX 400, Bybit 50) have count semantics instead:
after each message, truncate to the subscribed depth (Kraken requires this for its
checksum). OKX and Bybit [LIVE] never exceeded their depth because they send deletes for
levels leaving the window, but we still truncate defensively.

**Why not a price-indexed ladder?**
- A ladder gives O(1) updates, but at BTC's 0.01 tick a ±2 % band is ~320k slots
  (~2.5 MB of qty per side for Coinbase). Finding the next best level after the best is
  deleted needs a bitmap scan.
- Recentering is complicated.
- Kraken's checksum and top-N imbalance both need ordered iteration, which the array
  gives for free.

With 85 % of updates within 100 levels of the top, the array's expected `memmove` is
tiny. **Phase 1 benchmarks insert/modify/delete at ranks 0, 10, 100, 1000, and Cap.**
If the deep-rank cost turns out to be unacceptable, the ladder goes behind the same
interface for Coinbase only.

**Why no `std::map`:** every node is a separate heap allocation (which breaks the
zero-allocation rule), and each step of a traversal is a dependent pointer chase to a
cache line that's probably cold.

### 5.3 SPSC ring buffer

```
template <class T, size_t Cap>          // Cap power of two, T trivially copyable
class SpscRing {
  alignas(kCacheLine) std::atomic<uint64_t> tail_;   // written by producer only
  alignas(kCacheLine) uint64_t head_cache_;          // producer's cached copy of head_
  alignas(kCacheLine) std::atomic<uint64_t> head_;   // written by consumer only
  alignas(kCacheLine) uint64_t tail_cache_;          // consumer's cached copy of tail_
  alignas(kCacheLine) std::array<T, Cap> slots_;
};
```
- Indices are monotonically increasing `uint64_t` (2⁶⁴ never wraps in practice), and the
  slot is `idx & (Cap-1)`. Full is `tail - head == Cap` and empty is `tail == head`, so
  no slot is wasted to tell the two apart.
- **Memory orders.** Each one is justified in a code comment too.
  - Producer: write the slot, then `tail_.store(t+1, release)`. The release makes the
    slot write visible before the new tail is.
  - Consumer: `tail_.load(acquire)` pairs with that release. Once it sees `t+1`, the slot
    contents are visible.
  - Consumer: read the slot, then `head_.store(h+1, release)`. The release orders the
    slot *read* before the store, so the producer can't overwrite a slot the consumer is
    still reading.
  - Producer: `head_.load(acquire)` pairs with the consumer's release.
  - Each side loads its *own* index `relaxed`, since only that thread ever writes it.
  - No `seq_cst` anywhere. A single-producer, single-consumer protocol needs only
    pairwise happens-before, not a single total order.
- **Cached opposite index:** the producer re-reads `head_` (a cross-core cache miss) only
  when its cached copy says the ring is full. The consumer does the same with `tail_`.
  In steady state each side touches only its own lines.
- **Batching:** `consume_all(f)` reads `tail_` once, drains everything available, and
  publishes `head_` once.
- **Full queue:** the producer (I/O thread) spins briefly. If the queue is still full
  after a configured timeout, the venue resyncs, because a book thread that far behind
  means our state is invalid anyway. The book thread never blocks.

### 5.4 Event (SPSC slot type)

The event is a 64-byte trivially copyable struct (one cache line on Linux, half a line on
M1):
- kind (`SnapshotBegin`, `Level`, `MessageEnd`, `VenueDown`)
- venue, instrument, side, epoch
- price, qty
- exchange timestamp
- `t_recv`, `t_parsed`
- checksum/sequence payload

A message becomes N `Level` events plus one `MessageEnd`. Integrity checks run at
`MessageEnd`, because Kraken's checksum is defined over the book *after* all levels in
the message are applied.

### 5.5 Consolidated book

- After each `MessageEnd`, take the top `M` (default 20) levels of every **LIVE** venue.
- Convert USDT venues' prices to USD:
  `px_usd = round_half_even(px_usdt × rate / 1e8)` in `__int128`.
- K-way merge into a fixed array of `(price, qty, venue)`.
- At 4 × 20 levels this recomputes in well under a microsecond, so incremental
  maintenance isn't worth its complexity.
- Levels aren't aggregated across venues, so the dashboard can color each row by venue.
- **USDT rate rule:** if the Kraken `USDT/USD` book is stale for more than `fx_max_age`
  (default 5 s), USDT venues are excluded from the consolidated book (and flagged
  `fx_stale`), but their own books stay visible.

### 5.6 Signals (computed on the book thread after each consolidated update)

All integer math, converted to double only in the publisher for display.
- **Cross-venue best bid/ask:** `max(bid)` and `min(ask)` over LIVE venues, plus the
  spread in price and bp. A negative spread is shown as "crossed across venues" (fees
  ignored).
- **Microprice:** `(bid·Qask + ask·Qbid) / (Qbid + Qask)`, per venue and consolidated.
- **Top-N imbalance:** `(ΣQbid − ΣQask)/(ΣQbid + ΣQask)` over N levels (config, default 10).
  Caveat: N *levels* covers different price distances on venues with different ticks. A
  bp-band variant is a later option.
- **Per-venue staleness:**
  - `now − last message`: connection liveness, which includes heartbeats.
  - `now − last book change`: data freshness.
  - `local_recv − exchange_ts`: one-way latency plus clock offset (§7).

### 5.7 Publisher hand-off

- `TripleBuffer<PublishSnapshot>`: three slots and one atomic index carrying a "fresh"
  bit.
- The writer fills its private slot and `exchange`s it in (acq_rel). The reader
  `exchange`s it out only when the fresh bit is set.
- The writer is wait-free and never blocks, and the reader always gets the newest
  complete snapshot.
- Rejected alternative: a seqlock. It needs racy reads of non-atomic data, which is UB in
  the C++ memory model and gets flagged by TSan unless every word is atomic.
- Histograms aren't copied through the triple buffer. They are single-writer arrays of
  `std::atomic<uint64_t>` written with `relaxed` load+store (not RMW, since there is one
  writer). The publisher reads them `relaxed`. A snapshot torn across buckets is fine
  for a display.

### 5.8 Venue health state machine

```
 DISABLED (config)
 DISCONNECTED ─connect→ CONNECTING ─ws open+subscribe→ SYNCING ─snapshot applied→ LIVE
     ▲                                                                            │
     └──── backoff (250 ms·2^k, cap 30 s, ±20 % jitter; reset after 60 s LIVE) ◀── STALE
           STALE triggers: seq gap, checksum mismatch, crossed book, parse error,
           queue overflow, liveness timeout, horizon exhausted, socket close/error
```
Only LIVE venues feed the consolidated book. **Resync = close the connection and
reconnect.** That's one mechanism for every venue, it's always correct, and it fits
Coinbase's per-connection sequence numbers. Per-symbol resubscribe would be an
optimization, deferred unless measurements call for it.
- OKX limits connections to 3/s per IP and subscribe ops to 480/hour per connection.
- Bybit limits connections to 500 per 5 minutes.
- The backoff above stays far below these.

## 6. Per-exchange protocol summary

Instruments:

| Canonical | Coinbase | Kraken | OKX | Bybit (spot) |
|---|---|---|---|---|
| BTC-USD | `BTC-USD` | `BTC/USD` | `BTC-USDT` ×fx | `BTCUSDT` ×fx |
| ETH-USD | `ETH-USD` | `ETH/USD` | `ETH-USDT` ×fx | `ETHUSDT` ×fx |
| fx USDT→USD | — | `USDT/USD` (depth 10) | — | — |

Coinbase's BTC-USD book is shared with BTC-USDC. Kraken's USD is fiat USD. We treat both
as USD.

### 6.1 Coinbase Advanced Trade — `level2`
- **URL** `wss://advanced-trade-ws.coinbase.com`. No JWT needed for market data.
  ([channels](https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/websocket/websocket-channels))
- **Subscribe:** one channel per message.
  `{"type":"subscribe","channel":"level2","product_ids":["BTC-USD","ETH-USD"]}` plus
  `{"type":"subscribe","channel":"heartbeats"}`. A subscribe must arrive within **5 s** of
  connecting or the server disconnects us.
  ([overview](https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/websocket/websocket-overview))
- **Messages:** `channel:"l2_data"`, `timestamp` (RFC3339, server send time),
  `sequence_num`, and `events[]{type: snapshot|update, product_id, updates[]{side: bid|offer, event_time, price_level, new_quantity}}`.
  - `new_quantity` is the new absolute size, not a delta. `"0"` removes the level.
    ([level2](https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/websocket/level2))
  - [LIVE] The snapshot is the whole book: 20,068 bids / 21,025 asks, in a 4.53 MB frame.
    Receive buffer: 16 MB, preallocated. Larger than that → resync and log an error.
- **Sequencing:** `sequence_num` is **per connection** ("Per-connection message sequence
  number") and shared by every channel on it. [LIVE] Heartbeats and l2_data interleave in
  one 0,1,2,… sequence.
  - Rule: each message must be `prev + 1`.
  - A gap (`>prev+1`) → STALE → reconnect.
  - Lower than `prev`: the docs say it "can be ignored or represent a message that has
    arrived out of order". We treat it as an error, because applying an older absolute
    quantity would corrupt the book.
- **Checksum:** none.
- **Liveness:** the `heartbeats` channel sends a message every 1 s with `heartbeat_counter`.
  Without it, "most channels close within 60–90 s when no updates arrive". Liveness
  timeout: 5 s without any message.
- **Exchange timestamps:** envelope `timestamp` (send) and per-update `event_time`.

### 6.2 Kraken Spot WS v2 — `book`
- **URL** `wss://ws.kraken.com/v2`.
  ([book](https://docs.kraken.com/api/docs/websocket-v2/book))
- **Subscribe:** `{"method":"subscribe","params":{"channel":"book","symbol":["BTC/USD","ETH/USD"],"depth":25,"snapshot":true}}`.
  - Allowed depths: 10/25/100/500/1000.
  - We pick 25: the checksum covers the top 10, and imbalance uses ≤ 20.
  - The ack arrives once per symbol.
- **Messages:** `{"channel":"book","type":"snapshot"|"update","data":[{symbol,bids[{price,qty}],asks[…],checksum,timestamp}]}`.
  - [LIVE] `price` and `qty` are **JSON numbers** that keep trailing zeros
    (`"qty":0.00006120`, `"price":80001.0`). We read them through simdjson's
    `raw_json_token()`, never `get_double()`.
  - An update can contain several changes to the same level, applied in order.
  - `qty` 0 deletes the level.
  - After applying the message, **truncate to the subscribed depth**. Kraken doesn't
    send deletes for levels pushed out of depth.
    ([checksum guide](https://docs.kraken.com/api/docs/guides/spot-ws-book-v2))
- **Sequencing:** none. The checksum is the gap detector: a missed update produces a
  mismatch on the next message with probability ≈ 1 − 2⁻³².
- **Checksum (CRC32) over the top 10 levels:**
  1. Take the asks, best (lowest) first. For each level, take price and qty as strings,
     remove the `.`, strip leading zeros, and append price then qty.
  2. Do the same for the bids, best (highest) first.
  3. Compute CRC32 over asks‑string + bids‑string and compare it as **unsigned 32‑bit**
     with `checksum`.

  The guide doesn't name the CRC variant.
  - [LIVE] **zlib/IEEE CRC32 (reflected poly 0xEDB88320) matched 1,755 of 1,755
    messages.**
  - Our implementation formats strings from fixed-point using the instrument's
    `price_precision` / `qty_precision` (config, checked at startup against Kraken's
    `instrument` channel).
  - ARMv8 `__crc32b/__crc32d` (`<arm_acle.h>`, `+crc`) implement exactly this polynomial
    (not CRC32C), with a table fallback.
- **Liveness:** app-level `{"method":"ping"}` → `pong`, with `time_in`/`time_out`
  ([ping](https://docs.kraken.com/api/docs/websocket-v2/ping)). We ping every 5 s, and
  the two server timestamps give an NTP-style offset estimate (§7).
- **Exchange timestamp:** `data[].timestamp` (RFC3339, µs).

### 6.3 OKX v5 — `books`
- **URL** `wss://ws.okx.com/ws/v5/public` (443. See D9).
  - Public channels need no login.
  - Connections: 3/s per IP. Subscribe/unsubscribe/login ops: 480/hour per connection.
- **Subscribe:** `{"op":"subscribe","args":[{"channel":"books","instId":"BTC-USDT"}]}`.
  - `books` = a 400-level snapshot, then incremental updates. [LIVE] Updates came about
    every 100 ms.
  - `books-l2-tbt` (10 ms) and `books50-l2-tbt` may need login/VIP tier **[VERIFY]**, so
    we use `books`.
- **Messages:** `{"arg":{…},"action":"snapshot"|"update","data":[{asks:[[px,sz,"0",numOrders]],bids:[…],ts,checksum,seqId,prevSeqId}]}`.
  - Prices and sizes are strings. `sz "0"` deletes the level.
  - `ts` is in ms.
- **Sequencing:**
  - The snapshot has `prevSeqId = -1` [LIVE].
  - An update is valid **iff `prevSeqId == last seqId`**. `seqId` itself jumps by
    arbitrary amounts [LIVE].
  - `seqId == prevSeqId` with empty arrays is a no-change keep-alive, and `seqId` can
    *decrease* after maintenance while the chain stays valid **[VERIFY: from third-party
    sources only; the official page couldn't be read, §9]**. Neither case counts as a
    gap.
  - Chain break → STALE → reconnect.
  - [LIVE] 580 messages, 0 breaks.
- **Checksum:** **deprecated since 2026-06-23, always 0.** Don't validate it (D1).
  ([changelog](https://www.okx.com/docs-v5/log_en/))
- **Liveness:** if nothing has been received for 25 s, send the text `ping` and expect
  `pong` back. The server drops connections that are idle for 30 s.
- **Exchange timestamp:** `ts` (ms, string).

### 6.4 Bybit v5 — `orderbook.{depth}.{symbol}` (spot)
- **URL** `wss://stream.bybit.com/v5/public/spot`.
  ([connect](https://bybit-exchange.github.io/docs/v5/ws/connect))
  - At most 10 args per subscribe request (spot).
  - Don't open more than 500 connections per 5 minutes per domain.
- **Subscribe:** `{"op":"subscribe","args":["orderbook.50.BTCUSDT","orderbook.50.ETHUSDT"]}`.
  Spot depths are 1/50/200/1000, pushed every 10/20/100/200 ms.
  ([orderbook](https://bybit-exchange.github.io/docs/v5/websocket/public/orderbook))
- **Messages:** `{"topic","ts","type":"snapshot"|"delta","data":{"s","b":[[px,sz]],"a":[[px,sz]],"u","seq"},"cts"}`.
  - Size `"0"` deletes. A new price inserts. An existing price is updated.
  - Top-level key order differs by product ("for Spot … `ts` appears before `type`"), so
    we use `find_field_unordered` for the envelope.
- **Sequencing:**
  - A `snapshot` at any time → reset the book.
  - `u == 1` → service restarted, treat it as a snapshot.
  - The docs **don't promise** that `u` is contiguous. [LIVE] it was +1 in 2,036 of 2,036
    deltas.
  - Our rule: `u != last_u + 1` → STALE → reconnect (conservative). We count any
    occurrences, and if long captures show legitimate jumps, we relax the rule to
    `u > last_u`.
  - `seq` is a cross-depth sequence, used only for diagnostics.
- **Checksum:** none.
- **Liveness:** send `{"op":"ping"}` every 20 s. The pong has `"ret_msg":"pong"`.
- **Exchange timestamps:** `ts` (system generated, ms) and `cts` (matching engine, ms).

### 6.5 Audit resync (optional, Phase 3)

For venues without a checksum (Coinbase, Bybit), every `audit_interval` (default off;
suggested 15 min) the service opens a *second* connection, takes a fresh snapshot, and
compares it against the live book over the overlapping depth. Any mismatch → STALE and
resync. This is how we'd catch a silent bug in our delta application.

## 7. Measurement

- **Clock:**
  - On arm64 we read `cntvct_el0` directly (`isb; mrs`, a few ns) and convert with
    `cntfrq_el0`. Elsewhere we fall back to `std::chrono::steady_clock`.
  - **Resolution is 41.7 ns on M1 (24 MHz, measured)** and ~40 ns on Ampere (25 MHz,
    [VERIFY] on the VM).
  - Any stage that is usually shorter than ~10 ticks (book update, signal compute) is
    reported two ways:
    1. a histogram of whole ticks (with the quantization stated), and
    2. the **batch average**: total ticks over N events ÷ N, measured in replay over
       ≥ 10⁶ events.

    We never print "12 ns p50" from a 41.7 ns clock.
- **Stages:**

  | Stage | From → to | Thread |
  |---|---|---|
  | receive | — | `t_recv`: after the WS frame is complete (after TLS decrypt) |
  | parse | `t_recv` → `t_parsed` | I/O |
  | queue | `t_parsed` → `t_dequeued` | cross-thread; both reads come from the same system counter, so they're comparable across cores |
  | book update | `t_dequeued` → `t_book` | book |
  | signal | `t_book` → `t_signal` | book |
  | publish hand-off | `t_signal` → `t_published` | book (triple-buffer write) |

  The publisher separately measures its JSON encode and send time. The browser can
  report its own render time. Neither is mixed into the pipeline numbers.
- **Histograms:** our own log-linear design (HDR-style: 2⁵ sub-buckets per power of two
  ≈ 3 % relative error, 1 tick … 2³⁶ ticks).
  - Fixed arrays; one histogram per (stage, thread).
  - Reported percentiles: p50, p99, p99.9, max.
- **Clock offset** (shown with explicit uncertainty, never as a precise number):
  - All venues: `d = local_wall_recv − exchange_ts` is *offset + one-way latency*. We
    show the rolling **minimum** over 60 s (closest to pure offset + minimum path delay)
    and the median, labelled exactly that way.
  - Kraken: the ping/pong `time_in`/`time_out` give an NTP-style offset with error bound
    ±(RTT − server time)/2.
  - The VM runs chrony. Its reported estimated error is shown alongside.
- **Record / replay:** `.cbrec` binary log, little-endian.
  - **Header:** magic `CBREC001`, version, start wall time, venue/instrument table, and
    the config hash.
  - **Records:** `{u32 len, u8 venue, u8 kind(frame|connect|disconnect|subscribe_sent), u16 conn_epoch, u64 t_recv_ticks, u64 t_wall_ns, payload[len]}`.
  - Planned: a recorder thread fed from a per-venue SPSC byte ring. Built instead: the I/O
    threads write directly through a mutex-guarded, 8 MiB-buffered `FILE*` (§13.6).
  - Replay feeds frames through the same parsers. Inline mode is deterministic: the same
    file gives the same final books, bit for bit.
  - **All README numbers come from replay.**
- **Allocation proof:** the test binary replaces global `operator new`/`delete` with
  counting versions. It warms up on the first part of a replay, then asserts **0
  allocations** over the rest (parse → book → consolidated → signals → triple-buffer
  write).
  - Scope note: this doesn't see `malloc` from C libraries (OpenSSL) or kernel buffers.
  - The live network path (Asio/Beast/OpenSSL) is checked separately in a 60 s live run
    that also hooks `malloc` via interposition. Its result is reported as-is, even if
    it's not zero.

## 8. Dependencies (pinned)

| Dependency | Version | How | Why |
|---|---|---|---|
| simdjson | **v5.0.5** (2026-10-14) | FetchContent `GIT_TAG v5.0.5` | On-Demand parser. Releases v5.0.6–v5.0.8 appeared in the last 3 days, so we pin the newest one with two weeks of soak. |
| Boost (Beast, Asio, headers only) | **1.92.0** | FetchContent URL `boost-1.92.0-cmake.tar.xz`, SHA256 `9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d`, `BOOST_INCLUDE_LIBRARIES=beast` | One pinned version on both OSes. Ubuntu 24.04 apt ships 1.83, Homebrew ships 1.92. |
| OpenSSL | system 3.x (macOS: Homebrew `openssl@3`; CMake found **3.6.1**; Ubuntu 24.04: `libssl-dev` 3.0.x) | `find_package(OpenSSL 3)` | TLS for Asio. We deliberately don't vendor it: the distro gets security patches. |
| GoogleTest | **v1.18.0** | FetchContent | Unit and golden tests. |
| Google Benchmark | **v1.10.0** | FetchContent | Microbenchmarks. |
| zlib | system (macOS SDK / `zlib1g-dev`) | `find_package(ZLIB)` | Reads gzip-compressed recordings (the golden test data is committed compressed: 11.0 MB → 1.06 MB). |
| CMake | **≥ 3.25** | — | Needed for `FetchContent_Declare(... SYSTEM)`, which keeps our `-Werror` warning set out of third-party headers (§13.8). |
| Python 3 + `websockets` 13.1 | dev only | — | The protocol probes in `docs/probes/` and `tools/golden_reference.py`. Not used by the service. |

**Why Beast instead of uWebSockets:**
- We need a TLS WebSocket *client* for four venues. uWS is a server library (its client
  is not production-ready), so choosing uWS would still mean adding Beast or
  libwebsockets for the client side.
- Beast gives us one dependency and one event-loop model (Asio) for both sides, and
  control over buffers: a reused `flat_buffer` doesn't allocate in steady state.
- uWS's server is faster, but the publisher sends ~20 msgs/s to a handful of browsers, so
  server speed doesn't matter here.
- Cost: Beast is heavy to compile. We confine it to 2 TUs (`ws_client.cpp`,
  `ws_server.cpp`) with Asio/Beast separate compilation, and limit heavy jobs with a
  Ninja job pool (`-j` 2 for those TUs) so builds fit in 8 GB.

**Why not IXWebSocket or libwebsockets:**
- IXWebSocket allocates a `std::string` per message and runs its own threads.
- libwebsockets is a large C library with a callback model that's hard to fit around our
  own threads.

**simdjson padding:** On-Demand reads up to `SIMDJSON_PADDING` (64 bytes in v5.0.5)
past the end of the input.
- Receive buffers are allocated with that slack, and frames are parsed in place via
  `padded_string_view(data, len, capacity)`.
- Each I/O thread owns one `ondemand::parser` with `allocate(max_frame)` called up front
  (16 MB for Coinbase, 1 MB for the others), so parsing never grows its buffers.

## 9. Things I could not verify from official docs — please check, or I'll check empirically

1. **OKX `books` channel page:** I couldn't read it, because the docs are one ~1 MB page
   and the fetch tool truncates before the order-book section. I still need to confirm:
   - the exact push cadence and depth of `books`;
   - the official wording for the `seqId == prevSeqId` keep-alive and for `seqId`
     decreasing after maintenance;
   - whether `books-l2-tbt`/`books50-l2-tbt` need login and a VIP tier.

   **Please open** https://www.okx.com/docs-v5/en/#order-book-trading-market-data-ws-order-book-channel
   and compare it with §6.3. The live behaviour already matches.
2. **Bybit `u` contiguity:** the docs don't state it. It was contiguous in a 60 s live
   sample. Phase 3 will log `u` deltas over a multi-hour capture before we finalize the
   gap rule.
3. **Coinbase:** maximum message size and snapshot depth aren't documented (observed full
   book, 4.5 MB). Also, the docs call level2 "guaranteed delivery" but the overview warns
   about dropped messages. We handle gaps regardless.
4. **Kraken:** whether book numbers can ever appear in exponent form (`1e-8`). None were
   observed, and the parser rejects them, which forces a resync. Also unconfirmed: the
   heartbeat channel cadence (not on the pages I read). We rely on our own ping.
5. **Ampere `cntfrq_el0`** (~25 MHz expected) and **geo-access from OCI India IP ranges**
   to all four venues. The same probe will be rerun on the VM before deployment. All four
   worked from an Indian residential IP today.

## 10. Repository layout

```
CMakeLists.txt  CMakePresets.json        # presets: debug, release, asan-ubsan, tsan
cmake/          Dependencies.cmake  Sanitizers.cmake  Warnings.cmake
config/         default.json             # venues on/off, symbols, depths, scales, ports, cpu ids
src/
  core/         fixed_point.hpp  decimal_parse.hpp  spsc_ring.hpp  triple_buffer.hpp
                clock.hpp  histogram.hpp  crc32.hpp
  platform/     cacheline.hpp  thread.hpp/.cpp
  book/         book_side.hpp  venue_book.hpp  consolidated_book.hpp
  venue/        event.hpp  venue_health.hpp  backoff.hpp
                coinbase/  kraken/  okx/  bybit/      # parser + protocol state + checksum
  net/          ws_client.hpp/.cpp                    # Beast TLS client (the only Beast TU on the feed side)
  engine/       book_engine.hpp/.cpp  signals.hpp  fx.hpp
  record/       cbrec_format.hpp  recorder.cpp  replay_source.cpp
  publish/      publisher.cpp  ws_server.cpp  snapshot_json.cpp
  app/          main.cpp  (live | record | replay)
web/            index.html  app.js                    # vanilla JS + canvas
tests/          unit/  golden/  alloc/  data/*.cbrec
bench/          bench_spsc.cpp  bench_decimal.cpp  bench_book.cpp  bench_pipeline.cpp
deploy/         Dockerfile  consolidated-book.service  Caddyfile
.github/workflows/ci.yml                              # macos-14 (arm64) + ubuntu-24.04-arm: release, asan+ubsan, tsan
docs/           DESIGN.md  INTERVIEW_NOTES.md  DEPLOY.md  probes/
```

Config is JSON, parsed with simdjson (cold path), so we don't need a TOML/YAML
dependency.

## 11. Phase 2 venue recommendation: **Kraken**

- **It's the only venue with a working per-message checksum.** The first end-to-end venue
  gets a built-in correctness oracle: every one of our book states is verified by the
  exchange itself. Bugs in the fixed-point parser, the book, or truncation show up
  immediately, not as silent drift.
- It's depth-limited (small, fast-to-test messages) and it also carries the
  `USDT/USD` FX book we need anyway.
- Its weakness, no sequence numbers, is fully covered by the checksum.
- Coinbase would be the worst first choice: a 4.5 MB snapshot, horizon logic, and no
  checksum to validate against.

## 12. Toolchain notes

- macOS: Apple clang 21.0.0 (verified), CMake 4.3.3 (verified).
  - **Ninja isn't installed:** `brew install ninja` is recommended for job pools.
    Without it, use `cmake --build -j4`.
- Linux: Ubuntu 24.04 arm64, clang-18 or gcc-14 from apt.
- We avoid C++20 library features that are patchy on Apple libc++ (e.g. `std::jthread`
  before LLVM 18/20) and use `std::thread` with an atomic stop flag.
- CI runs natively on arm64 for both OSes (`macos-14`, `ubuntu-24.04-arm`), so there's no
  x86 or emulation gap between CI and the deployment target.
- Verified 2026-10-29: the Docker image builds with **GCC 13.3.0** on linux/aarch64 and
  passes 57/57 tests. GCC reports `-Wnull-dereference` inside Boost headers (a middle-end
  warning that `SYSTEM` can't suppress), so `-Werror` CI jobs use Clang and the GCC job
  reports warnings without failing on them.


## 13. Implementation notes: where measurements changed the plan

Every number below comes from a command in `README.md` or `docs/probes/`, run on the
Apple M1 dev machine on 2026-10-29.

### 13.1 Coinbase horizon size
A live count of Coinbase snapshot levels within ±X% of mid gave: BTC-USD ±2% = 1,791 bids /
594 asks, ±5% = 4,153 / 1,775; ETH-USD ±2% = 445 / 353. The default horizon is therefore
±2% with 4,096 slots per side. Wider bands raise the worst-case insert shift, and narrower
ones resync more often when the price trends.

### 13.2 Linear-scan window → pure binary search
`bench_book` showed the linear window winning only at rank ≈ 0 of small books (2.3 ns vs
4.6 ns) and losing past the window (28 vs 21 ns at rank 100 of 20k). On the measured
Coinbase rank mix (`bench_pipeline_synthetic`), pure binary search was fastest (74.6 ns vs
80–82 ns for windows 8–64), so it is the default. The window is still a template parameter.

### 13.3 Batch publication in the SPSC ring
`bench_spsc` measured 25.6 M items/s when publishing per item, but 84.8 / 95.3 / 400.6 M
items/s when publishing per batch of 4 / 16 / 64. The payload size didn't matter (64-byte
items were *faster* than 8-byte ones). The cost is the `tail_` cache line moving between
cores once per publication. Hence the `EventSink` stages a whole venue message and commits
it with one `try_push_batch`.

### 13.4 USDT conversion without 128-bit division
The first consolidated-book version converted prices with an `__int128` division
(`__divti3`, a library call on arm64) inside the merge loop: signal stage mean 1,155 ns.
Splitting `price = whole·1e8 + frac` keeps every product in int64 and gives identical
results (tested against the 128-bit reference on 10⁶ random inputs). Together with caching
each venue's converted candidate, the signal stage dropped to 267 ns mean.

### 13.5 Kraken checksum digit formatting
`bench_checksum`: the full checksum took 434 ns, of which hardware CRC32 was only 28 ns (the
table version takes 941 ns). Building the digit strings cost the rest: a runtime-divisor
division per value plus one division per digit. Compile-time divisors (a `switch` over the
precision) and a two-digits-per-step lookup table brought it to 221 ns.

### 13.6 Recorder: mutex + stdio buffer instead of a recorder thread
Recording is a diagnostic mode, and published latency numbers come from replay, not from
recording runs. So the simplest correct design won: I/O threads append through a mutex
(uncontended at a few hundred frames/s) into an 8 MiB `setvbuf` buffer. A dedicated thread
fed by byte rings would only matter if recorded runs were used for latency, and they aren't.

### 13.7 Liveness must track bytes, not frames
The first live runs read whole WebSocket messages. Coinbase's 4.5 MB snapshot once took more
than 5 s to arrive, so the watchdog declared a healthy connection dead. The read loop now
uses `async_read_some` and refreshes the liveness stamp on every chunk; the exchange-data
timestamps still refer to the completed frame.

### 13.8 Build hygiene
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion … -Werror` applies to our targets
only. Dependencies are declared `SYSTEM` (CMake ≥ 3.25) so their headers don't trip it.
`__int128` is spelled through `__extension__` typedefs (`core/int128.hpp`) because GCC's
`-Wpedantic` rejects the bare keyword and Clang doesn't.

### 13.9 Independent reference for golden tests
The golden test's expected output is generated by `tools/golden_reference.py`, a separate
Python/`Decimal` implementation of the same protocol rules, not by the C++ code under test.
A snapshot produced by the code under test could only catch regressions; agreement between
two independent implementations also catches errors that were present from the start.
