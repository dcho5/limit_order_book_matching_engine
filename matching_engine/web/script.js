// script.js — Limit Order Book Simulator
//
// Pure in-browser price-time priority matching engine.
// Background market makers generate random order flow continuously;
// visitors can place their own orders through the form.

'use strict';

// ── In-browser matching engine ────────────────────────────────────────────────

let nextId      = 1;
let simTime     = 0;
let totalOrders = 0;
let totalTrades = 0;

// bids / asks: Map<price, {orders: Array<{id,qty}>, vol: number}>
// vol is maintained incrementally — O(1) reads, no per-render reduce (F5).
const bids       = new Map();
const asks       = new Map();
const orderIndex = new Map();  // id → {side, price}
const trades     = [];         // newest first, max 20

let midPrice = 100;

// Fill hooks — called whenever any maker order is filled.
// Signature: hook(makerId: number, fillQty: number)
const fillHooks = [];

// ── Matching engine helpers ───────────────────────────────────────────────────

// bestBidPrice / bestAskPrice: O(1) maintained variables (F6).
// null when the respective side is empty.
let bestBidPrice = null;
let bestAskPrice = null;

function bestBid() { return bestBidPrice; }
function bestAsk() { return bestAskPrice; }

function _recomputeBest(side) {
  if (side === 'bid') {
    bestBidPrice = bids.size === 0 ? null : Math.max(...bids.keys());
  } else {
    bestAskPrice = asks.size === 0 ? null : Math.min(...asks.keys());
  }
}

function _getOrCreateLevel(book, price) {
  if (!book.has(price)) book.set(price, { orders: [], vol: 0 });
  return book.get(price);
}

// Returns filled qty. Walks levels from best price inward without sorting (F4).
function matchAgainst(takerSide, limitPrice, qty) {
  const oppBook  = takerSide === 'buy' ? asks : bids;
  const oppSide  = takerSide === 'buy' ? 'ask' : 'bid';
  let   remaining = qty;

  while (remaining > 0) {
    // O(1) best-price lookup via maintained variable (F4 + F6)
    const levelPrice = takerSide === 'buy' ? bestAskPrice : bestBidPrice;
    if (levelPrice === null) break;
    if (limitPrice !== null) {
      if (takerSide === 'buy'  && levelPrice > limitPrice) break;
      if (takerSide === 'sell' && levelPrice < limitPrice) break;
    }

    const level = oppBook.get(levelPrice);
    while (level.orders.length > 0 && remaining > 0) {
      const maker = level.orders[0];
      const fill  = Math.min(remaining, maker.qty);
      maker.qty    -= fill;
      remaining    -= fill;
      level.vol    -= fill;   // O(1) volume update (F5)

      const aggressor = takerSide === 'buy' ? 'BUY' : 'SELL';
      trades.unshift({ t: simTime, price: levelPrice, qty: fill, aggressor });
      if (trades.length > 20) trades.pop();
      totalTrades++;

      // Notify watchers — isolated so a hook error cannot abort mid-fill (F3)
      for (const h of fillHooks) {
        try { h(maker.id, fill); } catch (err) { console.error('fillHook error:', err); }
      }

      if (maker.qty <= 0) {
        level.orders.shift();
        orderIndex.delete(maker.id);
      }
    }
    if (level.orders.length === 0) {
      oppBook.delete(levelPrice);
      _recomputeBest(oppSide);   // only recomputed on level deletion (F4)
    }
  }
  return qty - remaining;
}

// Returns {id: number|null, filledQty, restingQty}
// id is non-null only when a remainder rests in the book.
function submitLimit(side, price, qty) {
  totalOrders++;
  simTime++;
  const filledQty  = matchAgainst(side, price, qty);
  const restingQty = qty - filledQty;
  let id = null;
  if (restingQty > 0) {
    id = nextId++;
    const book  = side === 'buy' ? bids : asks;
    const level = _getOrCreateLevel(book, price);
    level.orders.push({ id, qty: restingQty });
    level.vol += restingQty;   // O(1) volume update (F5)
    orderIndex.set(id, { side, price });
    // Update best price if this level beats the current best (F6)
    if (side === 'buy') {
      if (bestBidPrice === null || price > bestBidPrice) bestBidPrice = price;
    } else {
      if (bestAskPrice === null || price < bestAskPrice) bestAskPrice = price;
    }
  }
  return { id, filledQty, restingQty };
}

// Returns {filledQty}
function submitMarket(side, qty) {
  totalOrders++;
  simTime++;
  const filledQty = matchAgainst(side, null, qty);
  return { filledQty };
}

function cancelOrder(id) {
  const info = orderIndex.get(id);
  if (!info) return false;
  const book  = info.side === 'buy' ? bids : asks;
  const level = book.get(info.price);
  if (level) {
    const idx = level.orders.findIndex(o => o.id === id);
    if (idx !== -1) {
      level.vol -= level.orders[idx].qty;   // O(1) volume update (F5)
      level.orders.splice(idx, 1);
    }
    if (level.orders.length === 0) {
      book.delete(info.price);
      _recomputeBest(info.side === 'buy' ? 'bid' : 'ask');   // (F6)
    }
  }
  orderIndex.delete(id);
  // Keep user order status in sync regardless of who called cancel (F2)
  const uo = userOrderMap.get(id);
  if (uo) { uo.status = 'CANCELED'; userOrderMap.delete(id); }
  return true;
}

// ── User order tracking ───────────────────────────────────────────────────────
// Each entry: {localId, restingId, side, type, price, origQty, filledQty, status}
// status: 'OPEN' | 'PARTIAL' | 'FILLED' | 'CANCELED' | 'UNFILLED'

const userOrders   = [];             // ordered by submission, newest appended last
const userOrderMap = new Map();      // restingId → entry (for O(1) fill updates)
let   userOrderSeq = 0;

// Register a fill hook that updates user order status in real time.
fillHooks.push((makerId, fillQty) => {
  const uo = userOrderMap.get(makerId);
  if (!uo || uo.status === 'CANCELED') return;
  uo.filledQty = Math.min(uo.filledQty + fillQty, uo.origQty);
  if (uo.filledQty >= uo.origQty) {
    uo.status = 'FILLED';
    userOrderMap.delete(makerId);
  } else {
    uo.status = 'PARTIAL';
  }
});

function addUserOrder(restingId, side, type, price, origQty, filledQty, restingQty) {
  let status;
  if (type === 'market') {
    // Market orders never rest; remainder is silently dropped (F10)
    status = filledQty >= origQty ? 'FILLED' : filledQty > 0 ? 'PARTIAL' : 'REJECTED';
  } else {
    status = restingQty === 0 ? 'FILLED' : filledQty > 0 ? 'PARTIAL' : 'OPEN';
  }

  const uo = { localId: userOrderSeq++, restingId, side, type, price, origQty, filledQty, status };
  userOrders.push(uo);
  if (userOrders.length > 20) userOrders.shift();
  if (restingId !== null) userOrderMap.set(restingId, uo);
  return uo;
}

// ── Price history ─────────────────────────────────────────────────────────────

const priceHistory = [];
const MAX_HISTORY  = 300;

function recordMid() {
  const bb = bestBid(), ba = bestAsk();
  if (bb === null || ba === null) return;
  priceHistory.push({ t: simTime, mid: (bb + ba) / 2 });
  if (priceHistory.length > MAX_HISTORY) priceHistory.shift();
}

// ── Pseudo-random workload ────────────────────────────────────────────────────

let seed = Date.now() & 0xffffffff;
function rand() {
  seed = (Math.imul(seed, 1664525) + 1013904223) | 0;
  return (seed >>> 0) / 0x100000000;
}
function randInt(lo, hi) { return lo + Math.floor(rand() * (hi - lo + 1)); }

function driftMid() {
  const r = rand();
  if      (r < 0.30) midPrice = Math.max(60,  midPrice - 1);
  else if (r > 0.70) midPrice = Math.min(160, midPrice + 1);
}

function generateBatch() {
  driftMid();
  const n = randInt(1, 5);
  for (let i = 0; i < n; i++) {
    const r = rand();
    if (r < 0.55) {
      const side   = rand() < 0.5 ? 'buy' : 'sell';
      const offset = randInt(0, 6);
      const price  = side === 'buy' ? midPrice - offset : midPrice + offset;
      submitLimit(side, Math.max(1, price), randInt(10, 250));
    } else if (r < 0.85) {
      // Exclude user resting orders from background cancels (F1)
      const ids = [...orderIndex.keys()].filter(id => !userOrderMap.has(id));
      if (ids.length > 0) {
        cancelOrder(ids[Math.floor(rand() * ids.length)]);
      }
    } else {
      submitMarket(rand() < 0.5 ? 'buy' : 'sell', randInt(10, 100));
    }
  }
  recordMid();
  render();
}

// ── Rendering ─────────────────────────────────────────────────────────────────

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
    .map(([p, lvl]) => [p, lvl.vol]);   // O(1) vol read (F5)
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
  for (const [p, lvl] of bids) all.set(p, { v: lvl.vol, side: 'bid' });   // O(1) (F5)
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
  document.getElementById('stat-resting').textContent = orderIndex.size.toLocaleString();
  const bb = bestBid(), ba = bestAsk();
  document.getElementById('stat-mid').textContent =
    bb !== null && ba !== null ? ((bb + ba) / 2).toFixed(1) : '—';
}

// ── User orders panel ─────────────────────────────────────────────────────────

function renderUserOrders() {
  const panel = document.getElementById('user-orders-panel');
  const list  = document.getElementById('user-orders-list');

  if (userOrders.length === 0) { panel.style.display = 'none'; return; }
  panel.style.display = '';

  // Newest first — iterate backwards to avoid allocating a reversed copy (F12)
  const rows = [];
  for (let i = userOrders.length - 1; i >= 0; i--) {
    const uo = userOrders[i];
    const sideCls  = uo.side === 'buy' ? 'uo-buy' : 'uo-sell';
    const priceTxt = uo.type === 'market' ? 'MKT' : `@ ${uo.price}`;
    const fillPct  = uo.origQty > 0 ? Math.min(100, (uo.filledQty / uo.origQty * 100)) : 100;
    const statusCls = `uo-status-${uo.status.toLowerCase()}`;

    // Cancel button only for open/partial resting orders still in the book
    const canCancel = (uo.status === 'OPEN' || uo.status === 'PARTIAL')
                      && uo.restingId !== null
                      && orderIndex.has(uo.restingId);

    const cancelBtn = canCancel
      ? `<button class="btn btn-cancel-order" data-id="${uo.restingId}" title="Cancel order">✕</button>`
      : `<span class="uo-no-cancel"></span>`;

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
      <span class="uo-status ${statusCls}">● ${uo.status}</span>
      ${cancelBtn}
    </div>`);
  }
  list.innerHTML = rows.join('');
}

// Event delegation — one listener handles all cancel buttons.
document.getElementById('user-orders-list').addEventListener('click', e => {
  const btn = e.target.closest('.btn-cancel-order');
  if (!btn) return;
  const id = parseInt(btn.dataset.id, 10);
  cancelOrder(id);
  const uo = userOrderMap.get(id);
  if (uo) { uo.status = 'CANCELED'; userOrderMap.delete(id); }
  render();
});

// ── Price chart (canvas) ──────────────────────────────────────────────────────

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

  // Grid lines
  ctx.strokeStyle = '#1e2d45'; ctx.lineWidth = 1;
  ctx.font = '10px Courier New, monospace'; ctx.fillStyle = '#5a6a80'; ctx.textAlign = 'left';
  for (let g = 0; g <= 4; g++) {
    const y = PAD_T + (g / 4) * cH;
    ctx.beginPath(); ctx.moveTo(PAD_L, y); ctx.lineTo(PAD_L + cW, y); ctx.stroke();
    ctx.fillText((hi - (g / 4) * (hi - lo)).toFixed(1), W - PAD_R + 6, y + 3);
  }

  // Gradient fill
  const grad = ctx.createLinearGradient(0, PAD_T, 0, PAD_T + cH);
  grad.addColorStop(0, rising ? 'rgba(0,210,106,0.22)' : 'rgba(255,71,87,0.22)');
  grad.addColorStop(1, 'rgba(0,0,0,0)');
  ctx.beginPath();
  ctx.moveTo(xOf(0), PAD_T + cH);
  priceHistory.forEach((p, i) => ctx.lineTo(xOf(i), yOf(p.mid)));
  ctx.lineTo(xOf(n - 1), PAD_T + cH);
  ctx.closePath(); ctx.fillStyle = grad; ctx.fill();

  // Price line
  ctx.beginPath();
  priceHistory.forEach((p, i) => {
    if (i === 0) ctx.moveTo(xOf(i), yOf(p.mid));
    else         ctx.lineTo(xOf(i), yOf(p.mid));
  });
  ctx.strokeStyle = lineColor; ctx.lineWidth = 1.5; ctx.lineJoin = 'round'; ctx.stroke();

  // Dashed crosshair at current price
  const lastY = yOf(mids[n - 1]);
  ctx.setLineDash([3, 3]); ctx.strokeStyle = lineColor;
  ctx.lineWidth = 0.8; ctx.globalAlpha = 0.5;
  ctx.beginPath(); ctx.moveTo(PAD_L, lastY); ctx.lineTo(PAD_L + cW, lastY); ctx.stroke();
  ctx.setLineDash([]); ctx.globalAlpha = 1;

  // Dot
  ctx.beginPath(); ctx.arc(xOf(n - 1), lastY, 3.5, 0, Math.PI * 2);
  ctx.fillStyle = lineColor; ctx.fill();

  // Current price label
  ctx.fillStyle = lineColor; ctx.font = 'bold 10px Courier New, monospace'; ctx.textAlign = 'left';
  ctx.fillText(mids[n - 1].toFixed(1), W - PAD_R + 6, lastY + 4);
}

// Debounced resize — avoids GPU texture reallocation on every drag pixel (F7)
let _resizeTimer;
window.addEventListener('resize', () => {
  clearTimeout(_resizeTimer);
  _resizeTimer = setTimeout(() => { resizeChart(); renderPriceChart(); }, 120);
});

// ── User order form ───────────────────────────────────────────────────────────

document.getElementById('u-type').addEventListener('change', function () {
  document.getElementById('price-field').style.display =
    this.value === 'market' ? 'none' : '';
});

// Auto-fill price with current mid when field receives focus and hasn't been
// manually edited — prevents default 100 being far from a drifted market (F8)
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

document.getElementById('user-order-form').addEventListener('submit', e => {
  e.preventDefault();
  _setFormError('');

  const side  = document.getElementById('u-side').value;
  const type  = document.getElementById('u-type').value;
  const price = parseInt(document.getElementById('u-price').value, 10);
  const qty   = parseInt(document.getElementById('u-qty').value,   10);

  // Validated input with inline error feedback (F9)
  if (isNaN(qty) || qty <= 0) { _setFormError('Quantity must be a positive integer.'); return; }
  if (type === 'limit' && (isNaN(price) || price <= 0)) { _setFormError('Limit price must be a positive integer.'); return; }

  let restingId, filledQty, restingQty;

  if (type === 'market') {
    const r = submitMarket(side, qty);
    restingId = null; filledQty = r.filledQty; restingQty = 0;
  } else {
    const r = submitLimit(side, price, qty);
    restingId = r.id; filledQty = r.filledQty; restingQty = r.restingQty;
  }

  addUserOrder(restingId, side, type, type === 'market' ? null : price, qty, filledQty, restingQty);
  recordMid();
  render();

  const btn = document.getElementById('u-submit');
  btn.textContent = '✓ Placed';
  btn.classList.add('btn-placed');
  setTimeout(() => { btn.textContent = 'Submit Order'; btn.classList.remove('btn-placed'); }, 700);
});

// ── Simulation controls ───────────────────────────────────────────────────────

let simInterval = null;
let simSpeed    = 300;

function startSim() {
  if (simInterval) return;
  simInterval = setInterval(generateBatch, simSpeed);
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

// ── Bootstrap ─────────────────────────────────────────────────────────────────

(function seedBook() {
  for (let depth = 1; depth <= 8; depth++) {
    submitLimit('buy',  midPrice - depth, randInt(100, 400));
    submitLimit('sell', midPrice + depth, randInt(100, 400));
  }
  recordMid();
})();

resizeChart();
render();
startSim();
