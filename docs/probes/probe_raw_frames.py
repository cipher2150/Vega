"""Phase 0 probe: connect to each venue, subscribe to its L2 book, dump raw frames.

Raw text is preserved (no json.loads on numbers) so we can see exactly how each
venue encodes prices/sizes on the wire.
"""
import asyncio, json, sys, time
import websockets

VENUES = {
    "coinbase": ("wss://advanced-trade-ws.coinbase.com",
                 [{"type": "subscribe", "channel": "heartbeats"},
                  {"type": "subscribe", "channel": "level2", "product_ids": ["BTC-USD"]}]),
    "kraken": ("wss://ws.kraken.com/v2",
               [{"method": "subscribe", "params": {"channel": "book", "symbol": ["BTC/USD"], "depth": 10}}]),
    "okx": ("wss://ws.okx.com/ws/v5/public",
            [{"op": "subscribe", "args": [{"channel": "books", "instId": "BTC-USDT"}]}]),
    "bybit": ("wss://stream.bybit.com/v5/public/spot",
              [{"op": "subscribe", "args": ["orderbook.50.BTCUSDT"]}]),
}

async def probe(name, url, subs, n_frames=6, trunc=700):
    out = [f"===== {name} {url}"]
    try:
        async with websockets.connect(url, max_size=2**24, open_timeout=10) as ws:
            for s in subs:
                await ws.send(json.dumps(s))
            t0 = time.monotonic()
            got = 0
            while got < n_frames and time.monotonic() - t0 < 15:
                msg = await asyncio.wait_for(ws.recv(), timeout=15)
                got += 1
                out.append(f"[{name} #{got} len={len(msg)}] {msg[:trunc]}")
    except Exception as e:
        out.append(f"[{name}] ERROR {type(e).__name__}: {e}")
    return "\n".join(out)

async def main():
    which = sys.argv[1:] or list(VENUES)
    res = await asyncio.gather(*(probe(k, *VENUES[k]) for k in which))
    print("\n\n".join(res))

asyncio.run(main())
