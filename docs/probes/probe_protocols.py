"""Phase 0 empirical protocol checks (60 s live). Uses Decimal, never float.

coinbase: book size per side; rank-from-top of each updated level (drives book layout choice)
kraken:   validate CRC32 checksum algorithm on every message
okx:      seqId/prevSeqId chain continuity; book size vs 400
bybit:    u contiguity; book size vs subscribed depth
"""
import asyncio, json, sys, time, zlib, bisect
from decimal import Decimal
import websockets

DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0

def pct(xs, p):
    if not xs: return None
    xs = sorted(xs); return xs[min(len(xs) - 1, int(p / 100 * len(xs)))]

class Side:
    """Sorted price list + dict; rank 0 = best."""
    def __init__(self, desc): self.desc, self.px, self.q = desc, [], {}
    def key(self, p): return -p if self.desc else p
    def set(self, p, qty):
        k = self.key(p); i = bisect.bisect_left(self.px, k)
        rank = i
        if qty == 0:
            if i < len(self.px) and self.px[i] == k: self.px.pop(i); self.q.pop(p, None)
        else:
            if not (i < len(self.px) and self.px[i] == k): self.px.insert(i, k)
            self.q[p] = qty
        return rank
    def top(self, n): return [(self.key(k), self.q[self.key(k)]) for k in self.px[:n]]
    def truncate(self, n):
        for k in self.px[n:]: self.q.pop(self.key(k), None)
        del self.px[n:]

async def coinbase(res):
    bids, asks, ranks, sizes, seq_gaps, last = Side(True), Side(False), [], None, 0, None
    async with websockets.connect("wss://advanced-trade-ws.coinbase.com", max_size=2**25) as ws:
        await ws.send(json.dumps({"type": "subscribe", "channel": "heartbeats"}))
        await ws.send(json.dumps({"type": "subscribe", "channel": "level2", "product_ids": ["BTC-USD"]}))
        t0 = time.monotonic(); n_upd_msgs = 0
        while time.monotonic() - t0 < DUR:
            m = json.loads(await ws.recv(), parse_float=Decimal)
            s = m.get("sequence_num")
            if last is not None and s is not None and s != last + 1: seq_gaps += 1
            if s is not None: last = s
            if m.get("channel") != "l2_data": continue
            for ev in m["events"]:
                for u in ev["updates"]:
                    side = bids if u["side"] == "bid" else asks
                    r = side.set(Decimal(u["price_level"]), Decimal(u["new_quantity"]))
                    if ev["type"] == "update": ranks.append(r)
                if ev["type"] == "snapshot": sizes = (len(bids.px), len(asks.px))
                else: n_upd_msgs += 1
    res["coinbase"] = dict(snapshot_levels_bid_ask=sizes, final_levels=(len(bids.px), len(asks.px)),
        level_updates=len(ranks), update_msgs=n_upd_msgs, seq_gaps=seq_gaps,
        rank_p50=pct(ranks, 50), rank_p90=pct(ranks, 90), rank_p99=pct(ranks, 99), rank_max=max(ranks or [0]),
        frac_rank_lt_100=sum(r < 100 for r in ranks) / max(1, len(ranks)),
        frac_rank_lt_1000=sum(r < 1000 for r in ranks) / max(1, len(ranks)))

def kraken_fmt(raw):  # raw JSON number text, e.g. "0.00006120"
    return raw.replace(".", "").lstrip("0")

async def kraken(res):
    # Keep raw number text: hook parse_float/parse_int to retain the token string.
    ok = bad = 0; bids, asks = Side(True), Side(False); raw = {}
    depth = 10
    async with websockets.connect("wss://ws.kraken.com/v2") as ws:
        await ws.send(json.dumps({"method": "subscribe", "params": {"channel": "book", "symbol": ["BTC/USD"], "depth": depth}}))
        t0 = time.monotonic()
        while time.monotonic() - t0 < DUR:
            m = json.loads(await ws.recv(), parse_float=str, parse_int=str)
            if m.get("channel") != "book": continue
            for d in m["data"]:
                for side, lv in ((bids, d["bids"]), (asks, d["asks"])):
                    for l in lv:
                        p = Decimal(l["price"]); side.set(p, Decimal(l["qty"]))
                        raw[(id(side), p)] = (l["price"], l["qty"])
                bids.truncate(depth); asks.truncate(depth)
                s = "".join(kraken_fmt(raw[(id(asks), p)][0]) + kraken_fmt(raw[(id(asks), p)][1]) for p, _ in asks.top(10))
                s += "".join(kraken_fmt(raw[(id(bids), p)][0]) + kraken_fmt(raw[(id(bids), p)][1]) for p, _ in bids.top(10))
                if zlib.crc32(s.encode()) == int(d["checksum"]): ok += 1
                else: bad += 1
    res["kraken"] = dict(checksum_ok=ok, checksum_bad=bad)

async def okx(res):
    bids, asks, last, breaks, eq, dec, maxlv, n = Side(True), Side(False), None, 0, 0, 0, (0, 0), 0
    async with websockets.connect("wss://ws.okx.com/ws/v5/public", max_size=2**24) as ws:
        await ws.send(json.dumps({"op": "subscribe", "args": [{"channel": "books", "instId": "BTC-USDT"}]}))
        t0 = time.monotonic()
        while time.monotonic() - t0 < DUR:
            m = json.loads(await ws.recv())
            if "data" not in m: continue
            for d in m["data"]:
                if m["action"] == "snapshot": assert d["prevSeqId"] == -1
                elif d["prevSeqId"] != last: breaks += 1
                if d["seqId"] == d["prevSeqId"]: eq += 1
                if last is not None and d["seqId"] < last: dec += 1
                last = d["seqId"]; n += 1
                for side, lv in ((bids, d["bids"]), (asks, d["asks"])):
                    for l in lv: side.set(Decimal(l[0]), Decimal(l[1]))
                maxlv = (max(maxlv[0], len(bids.px)), max(maxlv[1], len(asks.px)))
    res["okx"] = dict(msgs=n, chain_breaks=breaks, seq_eq_prev=eq, seq_decreases=dec, max_levels_bid_ask=maxlv,
                      final_levels=(len(bids.px), len(asks.px)))

async def bybit(res):
    bids, asks, last, diffs, maxlv, n = Side(True), Side(False), None, {}, (0, 0), 0
    async with websockets.connect("wss://stream.bybit.com/v5/public/spot") as ws:
        await ws.send(json.dumps({"op": "subscribe", "args": ["orderbook.50.BTCUSDT"]}))
        t0 = time.monotonic()
        while time.monotonic() - t0 < DUR:
            m = json.loads(await ws.recv())
            if "data" not in m: continue
            d = m["data"]; u = d["u"]
            if m["type"] == "delta" and last is not None: diffs[u - last] = diffs.get(u - last, 0) + 1
            last = u; n += 1
            for side, lv in ((bids, d["b"]), (asks, d["a"])):
                for l in lv: side.set(Decimal(l[0]), Decimal(l[1]))
            maxlv = (max(maxlv[0], len(bids.px)), max(maxlv[1], len(asks.px)))
    res["bybit"] = dict(msgs=n, u_diff_histogram=diffs, max_levels_bid_ask_untruncated=maxlv)

async def main():
    res = {}
    async def run(f):
        try: await f(res)
        except Exception as e: res[f.__name__] = f"ERROR {type(e).__name__}: {e}"
    await asyncio.gather(*(run(f) for f in (coinbase, kraken, okx, bybit)))
    for k, v in res.items(): print(k, v)

asyncio.run(main())
