'use strict';
// Dashboard client: one WebSocket, two message types ("snap" at ~20 Hz, "lat" at 1 Hz).
// Prices/sizes arrive as exact decimal JSON numbers; they are only formatted here.

const VENUE_COLORS = {
  coinbase: '#3d7bf7', kraken: '#8e6cff', okx: '#c9d1d9', bybit: '#f7a600',
};
const css = getComputedStyle(document.documentElement);
const C = {
  bid: css.getPropertyValue('--bid').trim(), ask: css.getPropertyValue('--ask').trim(),
  text: css.getPropertyValue('--text').trim(), muted: css.getPropertyValue('--muted').trim(),
  line: css.getPropertyValue('--line').trim(),
};

let symbol = 'BTC-USD';
let lastSnap = null;
let lastLat = null;
let dirty = false;  // redraw only when something changed

// ---------------------------------------------------------------- formatting
const fmtPx = (v) => (v === undefined || v === 0 ? '–' : Number(v).toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 }));
const fmtQty = (v) => (v === undefined ? '–' : Number(v).toFixed(4));
const fmtNs = (ns) => (ns >= 1e6 ? (ns / 1e6).toFixed(2) + ' ms' : ns >= 1e3 ? (ns / 1e3).toFixed(1) + ' µs' : Math.round(ns) + ' ns');
const el = (id) => document.getElementById(id);

// ---------------------------------------------------------------- tabs & legend
function buildTabs() {
  const tabs = el('tabs');
  tabs.innerHTML = '';
  for (const s of ['BTC-USD', 'ETH-USD']) {
    const b = document.createElement('button');
    b.textContent = s;
    b.className = s === symbol ? 'active' : '';
    b.onclick = () => { symbol = s; buildTabs(); dirty = true; };
    tabs.appendChild(b);
  }
  el('legend').innerHTML = Object.entries(VENUE_COLORS)
    .map(([v, c]) => `<span style="margin-left:10px;text-transform:none;letter-spacing:0"><span class="swatch" style="background:${c}"></span>${v}</span>`)
    .join('');
}

// ---------------------------------------------------------------- canvas helpers
function fitCanvas(cv, cssHeight) {
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth;
  if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(cssHeight * dpr)) {
    cv.width = Math.round(w * dpr);
    cv.height = Math.round(cssHeight * dpr);
    cv.style.height = cssHeight + 'px';
  }
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w, h: cssHeight };
}

// ---------------------------------------------------------------- ladder
function drawLadder(cons) {
  const ROWS = 12;
  const rowH = 21;
  const { ctx, w, h } = fitCanvas(el('ladder'), rowH * (ROWS * 2 + 1) + 4);
  ctx.clearRect(0, 0, w, h);
  ctx.font = '12px ui-monospace, Menlo, monospace';
  ctx.textBaseline = 'middle';
  if (!cons) {
    ctx.fillStyle = C.muted;
    ctx.fillText('waiting for data…', 10, 20);
    return;
  }
  const asks = (cons.asks || []).slice(0, ROWS);
  const bids = (cons.bids || []).slice(0, ROWS);
  const maxQty = Math.max(1e-12, ...asks.map((r) => r[1]), ...bids.map((r) => r[1]));
  const labelW = 78;  // venue name column; bars must never cover it
  const priceX = Math.max(w * 0.52, labelW + 120);
  const barMax = priceX - 8 - labelW;

  const drawRow = (y, row, side) => {
    const [px, qty, venue] = row;
    const bw = Math.max(2, (qty / maxQty) * barMax);
    ctx.globalAlpha = 0.85;
    ctx.fillStyle = VENUE_COLORS[venue] || C.muted;
    ctx.fillRect(priceX - 8 - bw, y + 3, bw, rowH - 6);
    ctx.globalAlpha = 1;
    ctx.fillStyle = side === 'ask' ? C.ask : C.bid;
    ctx.textAlign = 'left';
    ctx.fillText(fmtPx(px), priceX, y + rowH / 2);
    ctx.fillStyle = C.muted;
    ctx.textAlign = 'right';
    ctx.fillText(fmtQty(qty), w - 4, y + rowH / 2);
    ctx.textAlign = 'left';
    ctx.fillText(venue, 4, y + rowH / 2);
  };

  // asks: worst at top, best just above the spread line
  for (let i = 0; i < asks.length; i++) drawRow((ROWS - 1 - i) * rowH, asks[i], 'ask');
  const midY = ROWS * rowH;
  ctx.strokeStyle = C.line;
  ctx.beginPath();
  ctx.moveTo(0, midY + rowH / 2);
  ctx.lineTo(w, midY + rowH / 2);
  ctx.stroke();
  ctx.fillStyle = C.text;
  ctx.textAlign = 'center';
  const spread = cons.spread !== undefined ? `spread ${Number(cons.spread).toFixed(2)}  (${cons.spread_bp} bp)` : '';
  ctx.fillText(spread, w / 2, midY + rowH / 2 - 1);
  ctx.textAlign = 'left';
  for (let i = 0; i < bids.length; i++) drawRow(midY + rowH + i * rowH, bids[i], 'bid');
}

// ---------------------------------------------------------------- signals
function renderSignals(cons) {
  const box = (k, v, s = '') => `<div class="sig"><div class="k">${k}</div><div class="v">${v}</div><div class="s">${s}</div></div>`;
  if (!cons || cons.bid === undefined) {
    el('signals').innerHTML = box('consolidated', '–', 'no live venues');
    return;
  }
  const imb = cons.imb;
  const imbColor = imb > 0 ? C.bid : C.ask;
  el('signals').innerHTML = [
    box('best bid', `<span style="color:${C.bid}">${fmtPx(cons.bid)}</span>`, cons.bid_venue),
    box('best ask', `<span style="color:${C.ask}">${fmtPx(cons.ask)}</span>`, cons.ask_venue),
    box('cross-venue spread', `${Number(cons.spread).toFixed(2)}`, `${cons.spread_bp} bp${cons.spread < 0 ? ' · crossed' : ''}`),
    box('microprice', fmtPx(cons.micro), 'size-weighted mid'),
    box('imbalance top-10', `<span style="color:${imbColor}">${(imb * 100).toFixed(1)}%</span>`, '(B−A)/(B+A)'),
    box('venues included', String(cons.venues.length), cons.venues.join(', ') || '–'),
  ].join('');
}

// ---------------------------------------------------------------- health
function renderHealth(snap) {
  const rows = [];
  rows.push('<tr><th>venue</th><th>book</th><th>conn</th><th>state</th><th>bid</th><th>ask</th><th>micro</th><th>imb</th><th>levels</th><th>age</th><th>lag / min</th><th>msgs</th><th>checksum ok/fail</th><th>resyncs</th><th>rx age</th><th>io allocs</th></tr>');
  for (const v of snap.venues) {
    const ntp = v.ntp_rtt_ms !== undefined ? ` · ntp ${v.ntp_offset_ms} ± ${(v.ntp_rtt_ms / 2).toFixed(1)} ms` : '';
    for (const b of v.books) {
      if (b.inst !== symbol && b.inst !== 'USDT-USD') continue;
      const ck = v.name === 'kraken' ? `${b.ck_ok} / ${b.ck_fail}` : 'n/a';
      const reason = b.state === 'live' ? '' : ` (${b.reason})`;
      rows.push(`<tr>
        <td><span class="swatch" style="background:${VENUE_COLORS[v.name]}"></span>${v.name}</td>
        <td>${b.inst}${b.quote === 'USDT' ? ' (USDT)' : ''}</td>
        <td class="${v.conn === 'connected' ? 'live' : v.conn}">${v.conn}${ntp}</td>
        <td class="${b.state}">${b.state}${reason}</td>
        <td>${fmtPx(b.bid)}</td><td>${fmtPx(b.ask)}</td><td>${fmtPx(b.micro)}</td>
        <td>${(b.imb * 100).toFixed(0)}%</td><td>${b.levels[0]}/${b.levels[1]}</td>
        <td>${b.age_ms < 0 ? '–' : b.age_ms.toFixed(0) + ' ms'}</td>
        <td>${b.lag_ms.toFixed(0)} / ${b.lag_min_ms.toFixed(0)} ms</td>
        <td>${b.msgs}</td><td>${ck}</td><td>${b.resyncs}</td>
        <td>${v.rx_age_ms < 0 ? '–' : v.rx_age_ms.toFixed(0) + ' ms'}</td><td>${v.io_allocs}</td></tr>`);
    }
  }
  el('health').innerHTML = rows.join('');
}

// ---------------------------------------------------------------- latency
function renderLatency(lat) {
  const host = el('hist');
  host.innerHTML = '';
  for (const s of lat.stages) {
    const cell = document.createElement('div');
    cell.className = 'cell';
    cell.innerHTML = `<div class="title"><span>${s.name}</span><span>n=${s.count}</span></div>
      <canvas height="70"></canvas>
      <div class="title"><span>p50 ${fmtNs(s.p50_ns)}</span><span>p99 ${fmtNs(s.p99_ns)}</span></div>
      <div class="title"><span>p99.9 ${fmtNs(s.p999_ns)}</span><span>max ${fmtNs(s.max_ns)}</span></div>
      <div class="title"><span>mean ${fmtNs(s.mean_ns)}</span><span></span></div>`;
    host.appendChild(cell);
    const { ctx, w, h } = fitCanvas(cell.querySelector('canvas'), 70);
    const bins = s.bins;
    let lo = bins.findIndex((c) => c > 0);
    let hi = bins.length - 1 - [...bins].reverse().findIndex((c) => c > 0);
    if (lo < 0) continue;
    lo = Math.max(0, lo - 1);
    hi = Math.min(bins.length - 1, hi + 1);
    const n = hi - lo + 1;
    const max = Math.max(...bins.slice(lo, hi + 1));
    const bw = w / n;
    ctx.font = '10px ui-monospace, Menlo, monospace';
    for (let i = 0; i < n; i++) {
      const c = bins[lo + i];
      // log scale so the tail (p99.9, max) stays visible next to the mode
      const bh = c === 0 ? 0 : Math.max(1, (Math.log10(c + 1) / Math.log10(max + 1)) * (h - 14));
      ctx.fillStyle = '#58a6ff';
      ctx.fillRect(i * bw + 1, h - 12 - bh, bw - 2, bh);
      if (i % Math.ceil(n / 6) === 0) {
        ctx.fillStyle = C.muted;
        ctx.fillText(fmtNs(2 ** (lo + i)).replace(' ', ''), i * bw + 1, h - 2);
      }
    }
  }
  el('clocknote').textContent =
    `Clock resolution ${lat.tick_ns} ns per tick (arm64 generic timer). Stages that take less than a few ticks ` +
    `(book, signal, publish) are quantized: trust "mean" (total ticks ÷ count) for those, not the percentiles. ` +
    `"receive" is when the WebSocket frame is complete in user space, after TLS decryption. Bars use log-scaled counts.`;
}

// ---------------------------------------------------------------- render loop
function render() {
  if (!lastSnap) return;
  const cons = lastSnap.cons.find((c) => c.sym === symbol);
  drawLadder(cons);
  renderSignals(cons);
  renderHealth(lastSnap);
  el('fx').textContent = `USDT/USD ${lastSnap.fx.live ? Number(lastSnap.fx.usdt_usd).toFixed(5) : 'stale'}`;
  el('fx').className = 'pill ' + (lastSnap.fx.live ? 'ok' : 'bad');
  el('seq').textContent = `msgs ${lastSnap.messages} · book allocs ${lastSnap.book_allocs ?? '–'}`;
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${proto}//${location.host}/ws`);
  ws.onopen = () => { el('conn').textContent = 'live'; el('conn').className = 'pill ok'; };
  ws.onclose = () => {
    el('conn').textContent = 'disconnected – retrying';
    el('conn').className = 'pill bad';
    setTimeout(connect, 1000);
  };
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.t === 'snap') { lastSnap = m; dirty = true; }
    else if (m.t === 'lat') { lastLat = m; renderLatency(m); }
  };
}

// Draw at most once per display frame, decoupled from message arrival (messages can burst).
function frame() {
  if (dirty) { dirty = false; render(); }
  requestAnimationFrame(frame);
}
window.addEventListener('resize', () => { dirty = true; if (lastLat) renderLatency(lastLat); });
buildTabs();
connect();
requestAnimationFrame(frame);
