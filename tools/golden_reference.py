#!/usr/bin/env python3
"""Independent reference implementation for golden-file tests.

Replays a .cbrec(.gz) recording with plain Python dicts and Decimal (no shared code with
the C++ service) and prints the final venue books in the exact format of
`cbook replay --dump`. If both implementations agree byte for byte, the C++ book logic,
fixed-point parser, depth truncation, horizon filter and sequence rules are confirmed by
a second, deliberately naive implementation.

Usage: python3 -I tools/golden_reference.py FILE.cbrec[.gz] > expected.txt
"""
import gzip
import json
import struct
import sys
import zlib
from decimal import Decimal

SCALE = 10**8
VENUES = ["coinbase", "kraken", "okx", "bybit"]
# venue -> [(symbol, instrument, price_precision, qty_precision)] in the C++ table order
INSTRUMENTS = {
    "coinbase": [("BTC-USD", "BTC-USD", 0, 0), ("ETH-USD", "ETH-USD", 0, 0)],
    "kraken": [("BTC/USD", "BTC-USD", 1, 8), ("ETH/USD", "ETH-USD", 2, 8), ("USDT/USD", "USDT-USD", 5, 8)],
    "okx": [("BTC-USDT", "BTC-USD", 0, 0), ("ETH-USDT", "ETH-USD", 0, 0)],
    "bybit": [("BTCUSDT", "BTC-USD", 0, 0), ("ETHUSDT", "ETH-USD", 0, 0)],
}
DEPTH = {"coinbase": None, "okx": 400, "bybit": 50}  # kraken: 25, USDT/USD 10
HORIZON_BP, MIN_LEVELS = 200, 20
SHUTDOWN_REASON = 11  # DownReason::kShutdown


def fixed(text):
    d = Decimal(str(text))
    v = d * SCALE
    if v != v.to_integral_value() or d < 0:
        raise ValueError(f"not representable: {text}")
    return int(v)


def fmt(v):
    whole, frac = divmod(v, SCALE)
    if frac == 0:
        return str(whole)
    return f"{whole}.{frac:08d}".rstrip("0")


class Book:
    def __init__(self):
        self.bids, self.asks, self.state = {}, {}, "empty"

    def side(self, s):
        return self.bids if s == "bid" else self.asks

    def apply(self, s, px, qty):
        side = self.side(s)
        if qty == 0:
            side.pop(px, None)
        else:
            side[px] = qty

    def top(self, s, n=None):
        keys = sorted(self.bids, reverse=True) if s == "bid" else sorted(self.asks)
        side = self.side(s)
        return [(k, side[k]) for k in (keys if n is None else keys[:n])]

    def truncate(self, depth):
        for s in ("bid", "ask"):
            keep = dict(self.top(s, depth))
            if s == "bid":
                self.bids = keep
            else:
                self.asks = keep


class Sim:
    def __init__(self):
        self.books = {(v, i[0]): Book() for v in VENUES for i in INSTRUMENTS[v]}
        self.min_epoch = {v: 0 for v in VENUES}
        self.epoch = {v: 0 for v in VENUES}
        self.failed = {v: True for v in VENUES}
        self.seq = {}

    # ---- engine-side rules ----
    def venue_stale(self, v):
        for i in INSTRUMENTS[v]:
            b = self.books[(v, i[0])]
            if b.state != "empty":
                b.state = "stale"

    def engine_fail(self, v):
        self.venue_stale(v)
        self.min_epoch[v] = max(self.min_epoch[v], self.epoch[v] + 1)

    def live_ok(self, v):
        return self.epoch[v] >= self.min_epoch[v]

    def message_end(self, v, sym, snapshot, checksum=None):
        b = self.books[(v, sym)]
        if b.state not in ("syncing", "live"):
            return
        depth = DEPTH.get(v) if v != "kraken" else (10 if sym == "USDT/USD" else 25)
        if depth:
            b.truncate(depth)
        if checksum is not None:
            pp, qp = next((i[2], i[3]) for i in INSTRUMENTS[v] if i[0] == sym)
            def digits(val, prec):
                q = Decimal(val) / SCALE
                return f"{q:.{prec}f}".replace(".", "").lstrip("0")
            s = "".join(digits(p, pp) + digits(q, qp) for p, q in b.top("ask", 10))
            s += "".join(digits(p, pp) + digits(q, qp) for p, q in b.top("bid", 10))
            if zlib.crc32(s.encode()) != checksum:
                return self.engine_fail(v)
        if b.bids and b.asks and max(b.bids) >= min(b.asks):
            return self.engine_fail(v)
        if v == "coinbase" and (len(b.bids) < MIN_LEVELS or len(b.asks) < MIN_LEVELS):
            return self.engine_fail(v)
        if b.state == "syncing" and snapshot:
            b.state = "live"

    def snapshot_begin(self, v, sym):
        b = self.books[(v, sym)]
        b.bids, b.asks, b.state = {}, {}, "syncing"

    def level(self, v, sym, side, px, qty):
        b = self.books[(v, sym)]
        if b.state in ("syncing", "live"):
            b.apply(side, px, qty)

    # ---- protocol-side rules ----
    def io_fail(self, v):
        self.failed[v] = True
        self.venue_stale(v)
        self.min_epoch[v] = max(self.min_epoch[v], self.epoch[v] + 1)

    def frame(self, v, payload):
        if self.failed[v]:
            return
        text = payload.decode()
        if v == "okx" and text == "pong":
            return
        handler = getattr(self, "on_" + v)
        ok = handler(json.loads(text, parse_float=Decimal))
        if ok is False:
            self.io_fail(v)

    def emit(self, v, events):
        """Apply a staged message's events if this session is still trusted."""
        if not self.live_ok(v):
            return
        for e in events:
            if not self.live_ok(v):
                return
            kind = e[0]
            if kind == "begin":
                self.snapshot_begin(v, e[1])
            elif kind == "lvl":
                self.level(v, *e[1:])
            else:
                self.message_end(v, *e[1:])

    def on_kraken(self, m):
        if m.get("channel") != "book" or "data" not in m:
            return True
        snap = m["type"] == "snapshot"
        for d in m["data"]:
            sym = d["symbol"]
            ev = [("begin", sym)] if snap else []
            for side, key in (("bid", "bids"), ("ask", "asks")):
                for l in d.get(key, []):
                    ev.append(("lvl", sym, side, fixed(l["price"]), fixed(l["qty"])))
            ev.append(("end", sym, snap, int(d["checksum"]) if "checksum" in d else None))
            self.emit("kraken", ev)
        return True

    def on_coinbase(self, m):
        s = m.get("sequence_num")
        last = self.seq.get("coinbase")
        if s is not None:
            if last is not None and s != last + 1:
                return False
            self.seq["coinbase"] = s
        if m.get("channel") != "l2_data":
            return True
        bands = {}
        for e in m["events"]:
            if e["type"] == "snapshot":
                bids = [fixed(u["price_level"]) for u in e["updates"] if u["side"] == "bid"]
                asks = [fixed(u["price_level"]) for u in e["updates"] if u["side"] != "bid"]
                mid = max(bids) // 2 + min(asks) // 2
                delta = mid * HORIZON_BP // 10000
                bands[e["product_id"]] = (mid - delta, mid + delta)
        for e in m["events"]:
            sym, snap = e["product_id"], e["type"] == "snapshot"
            if snap:
                self.seq[("cb_band", sym)] = bands[sym]
            floor, ceil = self.seq.get(("cb_band", sym), (None, None))
            ev = [("begin", sym)] if snap else []
            for u in e["updates"]:
                side = "bid" if u["side"] == "bid" else "ask"
                px, qty = fixed(u["price_level"]), fixed(u["new_quantity"])
                if floor is not None and ((side == "bid" and px < floor) or (side == "ask" and px > ceil)):
                    continue
                ev.append(("lvl", sym, side, px, qty))
            ev.append(("end", sym, snap, None))
            self.emit("coinbase", ev)
        return True

    def on_okx(self, m):
        if m.get("event") == "error":
            return False
        if "data" not in m:
            return True
        sym, snap = m["arg"]["instId"], m["action"] == "snapshot"
        for d in m["data"]:
            last = self.seq.get(("okx", sym))
            if not snap and (last is None or d["prevSeqId"] != last):
                return False
            self.seq[("okx", sym)] = d["seqId"]
            ev = [("begin", sym)] if snap else []
            for side, key in (("ask", "asks"), ("bid", "bids")):
                for l in d[key]:
                    ev.append(("lvl", sym, side, fixed(l[0]), fixed(l[1])))
            ev.append(("end", sym, snap, None))
            self.emit("okx", ev)
        return True

    def on_bybit(self, m):
        if "success" in m:
            return bool(m["success"])
        if "data" not in m:
            return True
        sym, snap, d = m["topic"].rsplit(".", 1)[1], m["type"] == "snapshot", m["data"]
        last = self.seq.get(("bybit", sym))
        if not snap and (last is None or d["u"] != last + 1):
            return False
        self.seq[("bybit", sym)] = d["u"]
        ev = [("begin", sym)] if snap else []
        for side, key in (("bid", "b"), ("ask", "a")):
            for l in d[key]:
                ev.append(("lvl", sym, side, fixed(l[0]), fixed(l[1])))
        ev.append(("end", sym, snap, None))
        self.emit("bybit", ev)
        return True

    def connect(self, v, epoch):
        self.epoch[v], self.failed[v] = epoch, False
        self.seq = {k: val for k, val in self.seq.items()
                    if not (k == v or (isinstance(k, tuple) and k[0] in (v, "cb_band" if v == "coinbase" else "")))}

    def disconnect(self, v):
        if not self.failed[v]:
            self.io_fail(v)


def main(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"CBREC001", "not a .cbrec file"
    pos, sim = 32, Sim()
    while pos + 32 <= len(data):
        length, venue, kind, flags, epoch, _r, _t, _w = struct.unpack_from("<IBBHIIQq", data, pos)
        pos += 32
        payload = data[pos:pos + length]
        pos += length
        if len(payload) < length:
            break
        v = VENUES[venue]
        if kind == 1:
            sim.connect(v, epoch)
        elif kind == 2:
            if flags != SHUTDOWN_REASON:
                sim.disconnect(v)
        else:
            sim.frame(v, payload)

    out = []
    for v in VENUES:
        for sym, _inst, _pp, _qp in INSTRUMENTS[v]:
            b = sim.books[(v, sym)]
            out.append(f"{v} {sym} {b.state} {len(b.bids)} {len(b.asks)}")
            out += [f"  B {fmt(p)} {fmt(q)}" for p, q in b.top("bid", 20)]
            out += [f"  A {fmt(p)} {fmt(q)}" for p, q in b.top("ask", 20)]
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1])
