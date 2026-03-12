// script.js — Limit Order Book Simulator
//
// Routes all engine calls through the C++ MatchingEngine compiled to WASM,
// or to the native engine_server (localhost:8765) if running.
// Rendering functions are unchanged; book state is populated from C++ JSON.

'use strict';

// ── Engine bridge ──────────────────────────────────────────────────────────────

let engineReady = false;
let useNative   = false;

function updateModeBadge() {
  const badge = document.getElementById('mode-badge');
  if (!badge) return;
  if (useNative) {
    badge.textContent = '● NATIVE C++';
    badge.className   = 'mode-badge mode-native';
  } else {
    badge.textContent = '● WASM';
    badge.className   = 'mode-badge mode-wasm';
  }
}

// Auto-detect native server; fall back to WASM silently
fetch('http://localhost:8765/health', { signal: AbortSignal.timeout(800) })
  .then(r => r.json())
  .then(d => { if (d.ok) { useNative = true; updateModeBadge(); if (!engineReady) startEngineInit(); } })
  .catch(() => { /* WASM */ });

// WASM init (via engine.js loaded in HTML)
if (window._wasmLoadFailed) {
  console.warn('engine.js not found — will use native server if available');
} else if (typeof Module !== 'undefined') {
  Module.onRuntimeInitialized = function () {
    engineReady = true;
    startEngineInit();
  };
}

function startEngineInit() {
  // Restore JS-side state saved before the last tab navigation
  const restored = tryRestoreState();

  if (useNative) {
    // Native server state persists across tab switches — never reset it.
    // Just sync current book state via a tick.
    generateBatch()
      .then(() => { if (!restored) recordMid(); render(); startSim(); })
      .catch(e => console.error('native tick failed:', e));
  } else {
    currentSessionIds.clear();
    Module._wasm_reset();
    generateBatch().then(() => { if (!restored) recordMid(); render(); startSim(); });
  }
}

// Unified engine call: routes to WASM or native server
async function callEngine(wasmFn, wasmArgs, nativePath, nativeBody) {
  if (useNative) {
    try {
      const r = await fetch('http://localhost:8765' + nativePath, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(nativeBody)
      });
      return r.json();
    } catch (_) {
      // Native server went away — fall back to WASM silently
      useNative = false;
      updateModeBadge();
    }
  }
  if (!engineReady) return null;
  const raw = Module[wasmFn](...wasmArgs);
  return JSON.parse(Module.UTF8ToString(raw));
}

// ── Book state (populated from C++ JSON) ──────────────────────────────────────

let simTime     = 0;
let totalOrders = 0;
let totalTrades = 0;
let bookResting = 0;   // from stats.resting in C++ response
let midPrice    = 100;

// bids/asks: Map<price, {orders: [], vol: number}>
// Kept in same shape as before so render functions work unchanged.
const bids       = new Map();
const asks       = new Map();
const orderIndex = new Map();  // id → {side, price} — populated from fills/submits
const trades     = [];

let bestBidPrice = null;
let bestAskPrice = null;

function bestBid() { return bestBidPrice; }
function bestAsk() { return bestAskPrice; }

// ── Apply C++ JSON response to local state ────────────────────────────────────

function applyBookState(data) {
  if (!data) return;

  // Rebuild bids/asks maps
  bids.clear();
  asks.clear();
  for (const b of (data.bids || [])) bids.set(b.price, { orders: [], vol: b.vol });
  for (const a of (data.asks || [])) asks.set(a.price, { orders: [], vol: a.vol });

  // Best prices
  bestBidPrice = data.bids && data.bids.length > 0 ? data.bids[0].price : null;
  bestAskPrice = data.asks && data.asks.length > 0 ? data.asks[0].price : null;

  // Stats
  if (data.stats) {
    totalOrders = data.stats.totalOrders;
    totalTrades = data.stats.totalTrades;
    bookResting = data.stats.resting;
    if (data.stats.mid) midPrice = data.stats.mid;
  }

  // Trades
  trades.length = 0;
  for (const t of (data.trades || [])) {
    trades.push({ t: t.t, price: t.price, qty: t.qty,
                  aggressor: t.side === 'buy' ? 'BUY' : 'SELL' });
  }
  simTime = trades.length > 0 ? trades[0].t : simTime;

  // Process fills → update user order status.
  // In WASM mode, only process fills from the current engine session; restored
  // orders from a previous session cannot receive fills from a fresh engine, and
  // new engine IDs can collide with old restingIds causing phantom status changes.
  for (const fill of (data.fills || [])) {
    if (!useNative && !currentSessionIds.has(fill.maker_id)) continue;
    const uo = userOrderMap.get(fill.maker_id);
    if (!uo || uo.status === 'CANCELED') continue;
    uo.filledQty = Math.min(uo.filledQty + fill.qty, uo.origQty);
    if (uo.filledQty >= uo.origQty) {
      uo.status = 'FILLED';
      userOrderMap.delete(fill.maker_id);
    } else {
      uo.status = 'PARTIAL';
    }
  }

  // Keep orderIndex for cancel-button visibility checks
  // We reconstruct from userOrderMap (only user orders need cancel buttons)
  orderIndex.clear();
  for (const [id, uo] of userOrderMap) {
    if (uo.status === 'OPEN' || uo.status === 'PARTIAL') {
      orderIndex.set(id, { side: uo.side, price: uo.price });
    }
  }
}

// ── Price history ──────────────────────────────────────────────────────────────

const priceHistory = [];
const MAX_HISTORY  = 300;

function recordMid() {
  if (bestBidPrice === null || bestAskPrice === null) return;
  const mid = (bestBidPrice + bestAskPrice) / 2;
  priceHistory.push({ t: simTime, mid });
  if (priceHistory.length > MAX_HISTORY) priceHistory.shift();
}

// ── User order tracking ────────────────────────────────────────────────────────

const userOrders      = [];
const userOrderMap    = new Map();  // restingId → entry
const currentSessionIds = new Set(); // WASM: restingIds placed in the current engine session
let   userOrderSeq    = 0;

function addUserOrder(restingId, side, type, price, origQty, filledQty, restingQty, elapsed_ns, avgFillPrice, midAtSubmission) {
  let status;
  if (type === 'market') {
    status = filledQty >= origQty ? 'FILLED' : filledQty > 0 ? 'PARTIAL' : 'REJECTED';
  } else {
    status = restingQty === 0 ? 'FILLED' : filledQty > 0 ? 'PARTIAL' : 'OPEN';
  }

  const uo = {
    localId: userOrderSeq++, restingId, side, type, price, origQty,
    filledQty, status, elapsed_ns: elapsed_ns || 0,
    avgFillPrice: avgFillPrice || 0,
    midAtSubmission: midAtSubmission || 0
  };
  userOrders.push(uo);
  if (userOrders.length > 50) userOrders.shift();
  if (restingId !== null) {
    userOrderMap.set(restingId, uo);
    if (!useNative) currentSessionIds.add(restingId);
  }
  return uo;
}

function computeAvgFillPrice(fills) {
  if (!fills || fills.length === 0) return 0;
  let totalQty = 0, totalValue = 0;
  for (const f of fills) { totalQty += f.qty; totalValue += f.qty * f.price; }
  return totalQty > 0 ? totalValue / totalQty : 0;
}

// ── Engine action wrappers ─────────────────────────────────────────────────────

async function submitLimit(side, price, qty) {
  const sideInt = side === 'buy' ? 0 : 1;
  return callEngine(
    '_wasm_submit_limit', [sideInt, price, qty],
    '/submit_limit', { side: sideInt, price, qty }
  );
}

async function submitMarket(side, qty) {
  const sideInt = side === 'buy' ? 0 : 1;
  return callEngine(
    '_wasm_submit_market', [sideInt, qty],
    '/submit_market', { side: sideInt, qty }
  );
}

async function cancelOrder(id) {
  if (useNative) {
    // Native: server retains full order state; cancel is authoritative.
    const data = await callEngine('_wasm_cancel', [id], '/cancel', { id });
    applyBookState(data);
    const uo = userOrderMap.get(id);
    if (uo) { uo.status = 'CANCELED'; userOrderMap.delete(id); }
  } else {
    // WASM: engine may have been reset since the order was placed (tab navigation).
    // Always update the UI regardless; the engine call is best-effort.
    try {
      const data = await callEngine('_wasm_cancel', [id], '/cancel', { id });
      applyBookState(data);
    } catch (_) {}
    const uo = userOrderMap.get(id);
    if (uo) { uo.status = 'CANCELED'; userOrderMap.delete(id); currentSessionIds.delete(id); }
  }
  orderIndex.delete(id);
  recordMid();
  render();
}

async function generateBatch() {
  const data = await callEngine(
    '_wasm_tick', [],
    '/tick', {}
  );
  applyBookState(data);
}

// ── Rendering ──────────────────────────────────────────────────────────────────

const BOOK_LEVELS  = 8;
const HEATMAP_ROWS = 14;
const TRADE_ROWS   = 10;

function render() {
  renderOrderBook();
  renderHeatmap();
  renderTrades();
  renderStats();
  renderPriceChart();
  renderUserOrders();
}

function renderOrderBook() {
  const topBids = [...bids.entries()]
    .sort((a, b) => b[0] - a[0]).slice(0, BOOK_LEVELS)
    .map(([p, lvl]) => [p, lvl.vol]);
  const topAsks = [...asks.entries()]
    .sort((a, b) => a[0] - b[0]).slice(0, BOOK_LEVELS)
    .map(([p, lvl]) => [p, lvl.vol]);

  while (topBids.length < BOOK_LEVELS) topBids.push(null);
  while (topAsks.length < BOOK_LEVELS) topAsks.push(null);

  const maxVol = Math.max(
    ...topBids.filter(Boolean).map(([, v]) => v),
    ...topAsks.filter(Boolean).map(([, v]) => v), 1
  );

  document.getElementById('bids-rows').innerHTML = topBids.map(entry => {
    if (!entry) return `<div class="level-row level-empty">
      <span class="lv-vol">—</span><div class="bar-wrap"></div><span class="lv-price">—</span>
    </div>`;
    const [price, vol] = entry;
    const pct = (vol / maxVol * 100).toFixed(0);
    return `<div class="level-row">
      <span class="lv-vol bid-text">${vol}</span>
      <div class="bar-wrap"><div class="bar bid-bar" style="width:${pct}%"></div></div>
      <span class="lv-price">${price}</span>
    </div>`;
  }).join('');

  document.getElementById('asks-rows').innerHTML = topAsks.map(entry => {
    if (!entry) return `<div class="level-row level-empty">
      <span class="lv-price">—</span><div class="bar-wrap"></div><span class="lv-vol">—</span>
    </div>`;
    const [price, vol] = entry;
    const pct = (vol / maxVol * 100).toFixed(0);
    return `<div class="level-row">
      <span class="lv-price">${price}</span>
      <div class="bar-wrap"><div class="bar ask-bar" style="width:${pct}%"></div></div>
      <span class="lv-vol ask-text">${vol}</span>
    </div>`;
  }).join('');

  const bb = topBids[0] ? topBids[0][0] : null;
  const ba = topAsks[0] ? topAsks[0][0] : null;
  const spreadEl = document.getElementById('spread-info');
  spreadEl.textContent = (bb !== null && ba !== null)
    ? `Spread: ${ba - bb}  ·  Mid: ${((bb + ba) / 2).toFixed(1)}`
    : '\u00a0';
}

function renderHeatmap() {
  const all = new Map();
  for (const [p, lvl] of bids) all.set(p, { v: lvl.vol, side: 'bid' });
  for (const [p, lvl] of asks) all.set(p, { v: lvl.vol, side: 'ask' });

  const prices = [...all.keys()].sort((a, b) => b - a).slice(0, HEATMAP_ROWS);
  const maxVol = Math.max(...prices.map(p => all.get(p).v), 1);

  const rows = prices.map(p => {
    const { v, side } = all.get(p);
    const pct     = (v / maxVol * 100).toFixed(0);
    const opacity = (0.25 + 0.75 * v / maxVol).toFixed(2);
    return `<div class="hm-row">
      <span class="hm-price">${p}</span>
      <div class="hm-bar ${side}-bar" style="width:${pct}%;opacity:${opacity}"></div>
      <span class="hm-vol">${v}</span>
    </div>`;
  });

  while (rows.length < HEATMAP_ROWS) {
    rows.push(`<div class="hm-row hm-row-empty">
      <span class="hm-price">—</span><div class="hm-bar"></div><span class="hm-vol"></span>
    </div>`);
  }

  document.getElementById('heatmap-rows').innerHTML = rows.join('');
}

function renderTrades() {
  const rows = trades.slice(0, TRADE_ROWS).map(t => {
    const cls = t.aggressor === 'BUY' ? 'trade-buy' : 'trade-sell';
    return `<tr class="${cls}">
      <td>${t.t}</td><td>${t.price}</td><td>${t.qty}</td><td>${t.aggressor}</td>
    </tr>`;
  });
  while (rows.length < TRADE_ROWS) {
    rows.push(`<tr class="trade-empty"><td>—</td><td>—</td><td>—</td><td>—</td></tr>`);
  }
  document.getElementById('trades-body').innerHTML = rows.join('');
}

function renderStats() {
  document.getElementById('stat-orders').textContent  = totalOrders.toLocaleString();
  document.getElementById('stat-trades').textContent  = totalTrades.toLocaleString();
  document.getElementById('stat-resting').textContent = bookResting.toLocaleString();
  const bb = bestBid(), ba = bestAsk();
  document.getElementById('stat-mid').textContent =
    bb !== null && ba !== null ? ((bb + ba) / 2).toFixed(1) : '—';
}

// ── User orders panel ──────────────────────────────────────────────────────────

function renderUserOrders() {
  const panel = document.getElementById('user-orders-panel');
  const list  = document.getElementById('user-orders-list');

  if (userOrders.length === 0) { panel.style.display = 'none'; return; }
  panel.style.display = '';

  const rows = [];
  for (let i = userOrders.length - 1; i >= 0; i--) {
    const uo = userOrders[i];
    const sideCls  = uo.side === 'buy' ? 'uo-buy' : 'uo-sell';
    const priceTxt = uo.type === 'market' ? 'MKT' : `@ ${uo.price}`;
    const fillPct  = uo.origQty > 0 ? Math.min(100, (uo.filledQty / uo.origQty * 100)) : 100;
    const statusCls = `uo-status-${uo.status.toLowerCase()}`;

    const canCancel = (uo.status === 'OPEN' || uo.status === 'PARTIAL')
                      && uo.restingId !== null;
    const cancelBtn = canCancel
      ? `<button class="btn btn-cancel-order" data-id="${uo.restingId}" title="Cancel order">✕</button>`
      : `<span class="uo-no-cancel"></span>`;

    // Fill time display
    const fillTime = uo.elapsed_ns > 0
      ? (useNative
          ? `${uo.elapsed_ns}ns`
          : `${Math.round(uo.elapsed_ns / 1000)}µs`)
      : '—';

    // Slippage: avg fill price vs limit price (limit orders) or mid at submission (market orders)
    let slippageTxt = '—';
    let slippageCls = '';
    if (uo.type === 'limit' && uo.avgFillPrice > 0 && uo.filledQty > 0) {
      const slip = uo.side === 'buy'
        ? uo.price - uo.avgFillPrice    // positive = favorable (paid less)
        : uo.avgFillPrice - uo.price;   // positive = favorable (received more)
      slippageTxt = slip === 0 ? '0' : (slip > 0 ? `+${slip.toFixed(1)}` : slip.toFixed(1));
      slippageCls = slip >= 0 ? 'slip-pos' : 'slip-neg';
    } else if (uo.type === 'market' && uo.avgFillPrice > 0 && uo.midAtSubmission > 0) {
      const slip = uo.side === 'buy'
        ? uo.midAtSubmission - uo.avgFillPrice
        : uo.avgFillPrice - uo.midAtSubmission;
      slippageTxt = slip === 0 ? '0' : (slip > 0 ? `+${slip.toFixed(1)}` : slip.toFixed(1));
      slippageCls = slip >= 0 ? 'slip-pos' : 'slip-neg';
    }

    rows.push(`<div class="uo-row">
      <span class="uo-side-badge ${sideCls}">${uo.side.toUpperCase()}</span>
      <span class="uo-type">${uo.type.toUpperCase()}</span>
      <span class="uo-price-label">${priceTxt}</span>
      <span class="uo-qty-label">× ${uo.origQty}</span>
      <div class="uo-bar-wrap">
        <div class="uo-bar ${uo.side === 'buy' ? 'bid-bar' : 'ask-bar'}"
             style="width:${fillPct.toFixed(0)}%"></div>
      </div>
      <span class="uo-fill-count">${uo.filledQty}/${uo.origQty}</span>
      <span class="uo-fill-time">${fillTime}</span>
      <span class="uo-slippage ${slippageCls}">${slippageTxt}</span>
      <span class="uo-status ${statusCls}">● ${uo.status}</span>
      ${cancelBtn}
    </div>`);
  }
  const header = `<div class="uo-row uo-col-header">
  <span>SIDE</span><span>TYPE</span><span>PRICE</span><span>QTY</span>
  <span>FILL</span><span>DONE</span><span>TIME</span><span>SLIP</span>
  <span>STATUS</span><span></span>
</div>`;
  list.innerHTML = header + rows.join('');
}

document.getElementById('user-orders-list').addEventListener('mousedown', e => {
  const btn = e.target.closest('.btn-cancel-order');
  if (!btn) return;
  e.preventDefault(); // prevent focus ring / text-selection side effects
  const id = parseInt(btn.dataset.id, 10);
  cancelOrder(id).catch(err => {
    console.error('Cancel failed:', err);
  });
});

// ── Price chart (canvas) ───────────────────────────────────────────────────────

const CHART_H = 140;

function resizeChart() {
  const canvas = document.getElementById('price-chart');
  const wrap   = canvas.parentElement;
  const dpr    = window.devicePixelRatio || 1;
  const logW   = wrap.clientWidth;
  canvas.width          = logW * dpr;
  canvas.height         = CHART_H * dpr;
  canvas.style.width    = logW + 'px';
  canvas.style.height   = CHART_H + 'px';
  canvas.getContext('2d').scale(dpr, dpr);
}

function renderPriceChart() {
  const canvas = document.getElementById('price-chart');
  const ctx    = canvas.getContext('2d');
  const dpr    = window.devicePixelRatio || 1;
  const W      = canvas.width  / dpr;
  const H      = canvas.height / dpr;

  ctx.clearRect(0, 0, W, H);

  if (priceHistory.length >= 1) {
    const last   = priceHistory[priceHistory.length - 1].mid;
    const first  = priceHistory[0].mid;
    const change = last - first;
    const pct    = first !== 0 ? ((change / first) * 100).toFixed(2) : '0.00';
    const sign   = change >= 0 ? '+' : '';
    document.getElementById('chart-cur-price').textContent = last.toFixed(1);
    const chEl = document.getElementById('chart-change');
    chEl.textContent = `${sign}${change.toFixed(1)} (${sign}${pct}%)`;
    chEl.className   = `chart-change ${change >= 0 ? 'ch-up' : 'ch-down'}`;
  }

  if (priceHistory.length < 2) return;

  const mids  = priceHistory.map(p => p.mid);
  const minP  = Math.min(...mids), maxP = Math.max(...mids);
  const range = Math.max(maxP - minP, 2);
  const pad   = range * 0.15;
  const lo = minP - pad, hi = maxP + pad;

  const PAD_L = 4, PAD_R = 46, PAD_T = 10, PAD_B = 6;
  const cW = W - PAD_L - PAD_R, cH = H - PAD_T - PAD_B;
  const n  = priceHistory.length;
  const xOf = i => PAD_L + (i / (n - 1)) * cW;
  const yOf = p => PAD_T + (1 - (p - lo) / (hi - lo)) * cH;

  const rising    = mids[n - 1] >= mids[0];
  const lineColor = rising ? '#00d26a' : '#ff4757';

  ctx.strokeStyle = '#1e2d45'; ctx.lineWidth = 1;
  ctx.font = '10px Courier New, monospace'; ctx.fillStyle = '#5a6a80'; ctx.textAlign = 'left';
  for (let g = 0; g <= 4; g++) {
    const y = PAD_T + (g / 4) * cH;
    ctx.beginPath(); ctx.moveTo(PAD_L, y); ctx.lineTo(PAD_L + cW, y); ctx.stroke();
    ctx.fillText((hi - (g / 4) * (hi - lo)).toFixed(1), W - PAD_R + 6, y + 3);
  }

  const grad = ctx.createLinearGradient(0, PAD_T, 0, PAD_T + cH);
  grad.addColorStop(0, rising ? 'rgba(0,210,106,0.22)' : 'rgba(255,71,87,0.22)');
  grad.addColorStop(1, 'rgba(0,0,0,0)');
  ctx.beginPath();
  ctx.moveTo(xOf(0), PAD_T + cH);
  priceHistory.forEach((p, i) => ctx.lineTo(xOf(i), yOf(p.mid)));
  ctx.lineTo(xOf(n - 1), PAD_T + cH);
  ctx.closePath(); ctx.fillStyle = grad; ctx.fill();

  ctx.beginPath();
  priceHistory.forEach((p, i) => {
    if (i === 0) ctx.moveTo(xOf(i), yOf(p.mid));
    else         ctx.lineTo(xOf(i), yOf(p.mid));
  });
  ctx.strokeStyle = lineColor; ctx.lineWidth = 1.5; ctx.lineJoin = 'round'; ctx.stroke();

  const lastY = yOf(mids[n - 1]);
  ctx.setLineDash([3, 3]); ctx.strokeStyle = lineColor;
  ctx.lineWidth = 0.8; ctx.globalAlpha = 0.5;
  ctx.beginPath(); ctx.moveTo(PAD_L, lastY); ctx.lineTo(PAD_L + cW, lastY); ctx.stroke();
  ctx.setLineDash([]); ctx.globalAlpha = 1;

  ctx.beginPath(); ctx.arc(xOf(n - 1), lastY, 3.5, 0, Math.PI * 2);
  ctx.fillStyle = lineColor; ctx.fill();

  ctx.fillStyle = lineColor; ctx.font = 'bold 10px Courier New, monospace'; ctx.textAlign = 'left';
  ctx.fillText(mids[n - 1].toFixed(1), W - PAD_R + 6, lastY + 4);
}

let _resizeTimer;
window.addEventListener('resize', () => {
  clearTimeout(_resizeTimer);
  _resizeTimer = setTimeout(() => { resizeChart(); renderPriceChart(); }, 120);
});

// ── User order form ────────────────────────────────────────────────────────────

document.getElementById('u-type').addEventListener('change', function () {
  document.getElementById('price-field').style.display =
    this.value === 'market' ? 'none' : '';
});

document.getElementById('u-price').addEventListener('focus', function () {
  const bb = bestBid(), ba = bestAsk();
  if (bb !== null && ba !== null)
    this.value = Math.round((bb + ba) / 2);
});

function _setFormError(msg) {
  const el = document.getElementById('form-error');
  el.textContent = msg;
  el.style.display = msg ? '' : 'none';
}

document.getElementById('user-order-form').addEventListener('submit', async e => {
  e.preventDefault();
  _setFormError('');

  const side  = document.getElementById('u-side').value;
  const type  = document.getElementById('u-type').value;
  const price = parseInt(document.getElementById('u-price').value, 10);
  const qty   = parseInt(document.getElementById('u-qty').value,   10);

  if (isNaN(qty) || qty <= 0) { _setFormError('Quantity must be a positive integer.'); return; }
  if (type === 'limit' && (isNaN(price) || price <= 0)) { _setFormError('Limit price must be a positive integer.'); return; }

  const midAtSubmission = midPrice;
  let data;
  if (type === 'market') {
    data = await submitMarket(side, qty);
  } else {
    data = await submitLimit(side, price, qty);
  }

  if (!data) { _setFormError('Engine not ready.'); return; }

  applyBookState(data);

  const fills    = data.fills || [];
  const elapsed  = data.elapsed_ns || 0;
  const avgFill  = computeAvgFillPrice(fills);

  let restingId = null, filledQty = 0, restingQty = 0;
  if (type === 'market') {
    filledQty = fills.reduce((s, f) => s + f.qty, 0);
  } else {
    // id in response is the resting order ID (0 if fully filled immediately)
    restingId   = data.id > 0 ? data.id : null;
    filledQty   = fills.reduce((s, f) => s + f.qty, 0);
    restingQty  = qty - filledQty;
  }

  addUserOrder(restingId, side, type, type === 'market' ? null : price,
               qty, filledQty, restingQty, elapsed, avgFill, midAtSubmission);

  recordMid();
  render();

  const btn = document.getElementById('u-submit');
  btn.textContent = '✓ Placed';
  btn.classList.add('btn-placed');
  setTimeout(() => { btn.textContent = 'Submit Order'; btn.classList.remove('btn-placed'); }, 700);
});

// ── Simulation controls ────────────────────────────────────────────────────────

let simInterval = null;
let simSpeed    = 300;

function startSim() {
  if (simInterval) return;
  simInterval = setInterval(async () => {
    await generateBatch();
    recordMid();
    render();
  }, simSpeed);
  document.getElementById('btn-pause').disabled  = false;
  document.getElementById('btn-resume').disabled = true;
}

function pauseSim() {
  clearInterval(simInterval);
  simInterval = null;
  document.getElementById('btn-pause').disabled  = true;
  document.getElementById('btn-resume').disabled = false;
}

document.getElementById('btn-pause').addEventListener('click',  pauseSim);
document.getElementById('btn-resume').addEventListener('click', startSim);

document.getElementById('speed').addEventListener('input', function () {
  simSpeed = parseInt(this.value, 10);
  document.getElementById('speed-val').textContent = `${simSpeed}ms`;
  if (simInterval) { pauseSim(); startSim(); }
});

// ── Session state — persist across tab navigation ──────────────────────────────

const SESSION_KEY = 'lobSession';

function saveState() {
  try {
    sessionStorage.setItem(SESSION_KEY, JSON.stringify({
      priceHistory,
      userOrders,
      simSpeed,
      totalOrders,
      totalTrades,
    }));
  } catch (_) { /* storage full or unavailable */ }
}

function tryRestoreState() {
  try {
    const raw = sessionStorage.getItem(SESSION_KEY);
    if (!raw) return false;
    const s = JSON.parse(raw);

    if (Array.isArray(s.priceHistory) && s.priceHistory.length > 0) {
      priceHistory.push(...s.priceHistory.slice(-MAX_HISTORY));
      const last = priceHistory[priceHistory.length - 1];
      if (last) {
        bestBidPrice = last.mid + 0.5;  // approximate until first tick
        bestAskPrice = last.mid + 0.5;
      }
    }

    if (Array.isArray(s.userOrders)) {
      userOrders.push(...s.userOrders);
      userOrderSeq = userOrders.length;
      // Rebuild userOrderMap only for orders that can still receive fills
      for (const uo of userOrders) {
        if (uo.restingId !== null &&
            (uo.status === 'OPEN' || uo.status === 'PARTIAL')) {
          userOrderMap.set(uo.restingId, uo);
        }
      }
    }

    if (s.simSpeed) {
      simSpeed = s.simSpeed;
      const slider = document.getElementById('speed');
      if (slider) { slider.value = simSpeed; }
      const label = document.getElementById('speed-val');
      if (label) label.textContent = simSpeed + 'ms';
    }

    if (s.totalOrders) totalOrders = s.totalOrders;
    if (s.totalTrades) totalTrades = s.totalTrades;

    return true;
  } catch (_) {
    return false;
  }
}

// Save before any navigation away from this page
window.addEventListener('beforeunload', saveState);

// ── Bootstrap ──────────────────────────────────────────────────────────────────

resizeChart();
render();

// If WASM is already initialised synchronously (unlikely but possible in some builds):
if (typeof Module !== 'undefined' && Module.calledRun) {
  engineReady = true;
  startEngineInit();
}
