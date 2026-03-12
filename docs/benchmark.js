// benchmark.js — Benchmark page controller
'use strict';

// F21: named constants replacing bare literals
const NATIVE_MAX_OPS    = 35_000_000;
const WASM_MAX_OPS      = 10_000_000;
const HEALTH_TIMEOUT_MS = 800;

// F20: named canvas colour constants
const CHART = {
  UP:    '#00d26a',
  DOWN:  '#ff4757',
  GRID:  '#1e2d45',
  BAR:   '#ffd32a99',
  LABEL: '#5a6a80'
};

let benchEngineReady = false;
let benchUseNative   = false;

// ── Mode detection ────────────────────────────────────────────────────────────

// shared: keep in sync with script.js (F15)
function updateModeBadge() {
  const badge = document.getElementById('mode-badge');
  if (!badge) return;
  if (benchUseNative) {
    badge.textContent  = '● NATIVE C++';
    badge.className    = 'mode-badge mode-native';
  } else {
    badge.textContent  = '● WASM';
    badge.className    = 'mode-badge mode-wasm';
  }
}

function applyOpsLimits() {
  const input = document.getElementById('bench-ops');
  if (benchUseNative) {
    input.max = NATIVE_MAX_OPS;  // F21
    if (!document.querySelector('[data-val="35000000"]')) {
      const pill = document.createElement('button');
      pill.className   = 'bench-ops-pill';
      pill.dataset.val = '35000000';
      pill.textContent = '35M';
      // F17/F23: listener on new pill reads live values at click time
      pill.addEventListener('click', function () {
        setPreset(NATIVE_MAX_OPS, 0, 0, 0);
      });
      document.querySelector('.bench-ops-pills').appendChild(pill);
    }
    const maxLoadBtn = document.querySelector('#bench-scenario-pills .bench-pill[data-ops="10000000"]');
    if (maxLoadBtn) {
      maxLoadBtn.dataset.ops = '35000000';
      maxLoadBtn.textContent = 'Max Load · 35M';
    }
    const opsNote = document.querySelector('.bench-ops-note');
    if (opsNote) {
      opsNote.textContent =
        'Min 10K: fewer ops give noisy averages. Max 35M: beyond this the order-ID map ' +
        'outgrows L3 cache and throughput falls — this reflects real sustained-load behavior.';
    }
  } else {
    input.max = WASM_MAX_OPS;  // F21
  }
}

// F16: shared health-check fetch — keep in sync with script.js
fetch('http://localhost:8765/health', { signal: AbortSignal.timeout(HEALTH_TIMEOUT_MS) })  // F21
  .then(r => r.json())
  .then(d => { if (d.ok) { benchUseNative = true; updateModeBadge(); } applyOpsLimits(); })
  .catch(() => { applyOpsLimits(); });

// Show download link (B11: corrected URL)
(function () {
  const note = document.getElementById('bench-download-note');
  if (note) {
    note.innerHTML = ' &mdash; <a href="https://github.com/dcho5/limit_order_book_matching_engine/releases" ' +
      'class="text-yellow">Download native server</a> for nanosecond-resolution timing (12–19M ops/sec standard mix).';
  }
})();

// ── Interactive controls ───────────────────────────────────────────────────────

function updateMixUI() {
  const lp = parseInt(document.getElementById('bench-limit-pct').value,  10);
  let   cp = parseInt(document.getElementById('bench-cancel-pct').value, 10);

  // Clamp cancel so limit + cancel ≤ 100
  const cpMax = 100 - lp;
  if (cp > cpMax) {
    cp = cpMax;
    document.getElementById('bench-cancel-pct').value = cp;
  }
  const mp = 100 - lp - cp;

  document.getElementById('bench-market-pct').value = mp;
  document.getElementById('bench-lval').textContent  = lp + '%';
  document.getElementById('bench-cval').textContent  = cp + '%';
  document.getElementById('bench-mval').textContent  = mp + '%';

  // Stacked bar
  document.getElementById('bench-mix-lseg').style.width = lp + '%';
  document.getElementById('bench-mix-cseg').style.width = cp + '%';
  document.getElementById('bench-mix-mseg').style.width = mp + '%';

  const lTxt = lp  > 10 ? `LIMIT ${lp}%`    : lp  > 5 ? `${lp}%`  : '';
  const cTxt = cp  > 10 ? `CANCEL ${cp}%`   : cp  > 5 ? `${cp}%`  : '';
  const mTxt = mp  > 10 ? `MKT ${mp}%`      : mp  > 5 ? `${mp}%`  : '';
  document.querySelector('#bench-mix-lseg .bench-mix-seg-txt').textContent = lTxt;
  document.querySelector('#bench-mix-cseg .bench-mix-seg-txt').textContent = cTxt;
  document.querySelector('#bench-mix-mseg .bench-mix-seg-txt').textContent = mTxt;

  // Market auto-fill bar
  document.getElementById('bench-mix-mbar').style.width = mp + '%';
}

// F23: setPreset reads live lp/cp/mp from sliders when called with 0/0/0
function setPreset(ops, lp, cp, mp) {
  const opsInput   = document.getElementById('bench-ops');
  const clampedOps = Math.min(ops, parseInt(opsInput.max, 10));
  opsInput.value   = clampedOps;
  document.getElementById('bench-ops-display').textContent = clampedOps.toLocaleString();

  // If lp/cp/mp are 0 (called from ops-only pills), read live slider values
  const liveLp = lp || parseInt(document.getElementById('bench-limit-pct').value,  10);
  const liveCp = cp || parseInt(document.getElementById('bench-cancel-pct').value, 10);
  const liveMp = mp || parseInt(document.getElementById('bench-market-pct').value, 10);

  document.getElementById('bench-limit-pct').value  = liveLp;
  document.getElementById('bench-cancel-pct').value = liveCp;
  document.getElementById('bench-market-pct').value = liveMp;
  updateMixUI();

  // Highlight matching ops pill
  document.querySelectorAll('.bench-ops-pill').forEach(p => {
    p.classList.toggle('bench-pill-active', parseInt(p.dataset.val, 10) === clampedOps);
  });

  // Highlight matching scenario pill
  document.querySelectorAll('#bench-scenario-pills .bench-pill').forEach(p => {
    p.classList.toggle('bench-pill-active',
      parseInt(p.dataset.ops, 10) === ops     &&
      parseInt(p.dataset.lp,  10) === liveLp  &&
      parseInt(p.dataset.cp,  10) === liveCp  &&
      parseInt(p.dataset.mp,  10) === liveMp);
  });
}

// Ops slider
document.getElementById('bench-ops').addEventListener('input', function () {
  const ops = parseInt(this.value, 10);
  document.getElementById('bench-ops-display').textContent = ops.toLocaleString();
  document.querySelectorAll('.bench-ops-pill').forEach(p => {
    p.classList.toggle('bench-pill-active', parseInt(p.dataset.val, 10) === ops);
  });
  document.querySelectorAll('#bench-scenario-pills .bench-pill').forEach(p => p.classList.remove('bench-pill-active'));
});

// F17: single event-delegation handler on the container instead of per-pill listeners.
// Covers all current and future .bench-ops-pill children, no duplicate listener risk.
document.querySelector('.bench-ops-pills').addEventListener('click', function (e) {
  const pill = e.target.closest('.bench-ops-pill');
  if (!pill) return;
  setPreset(parseInt(pill.dataset.val, 10), 0, 0, 0);
});

// F18: shared onMixChange handler — eliminates duplicate deactivation logic
function onMixChange() {
  updateMixUI();
  document.querySelectorAll('#bench-scenario-pills .bench-pill').forEach(p => p.classList.remove('bench-pill-active'));
}
document.getElementById('bench-limit-pct').addEventListener('input',  onMixChange);
document.getElementById('bench-cancel-pct').addEventListener('input', onMixChange);

// Scenario preset pills
document.querySelectorAll('#bench-scenario-pills .bench-pill').forEach(btn => {
  btn.addEventListener('click', function () {
    setPreset(
      parseInt(this.dataset.ops, 10),
      parseInt(this.dataset.lp,  10),
      parseInt(this.dataset.cp,  10),
      parseInt(this.dataset.mp,  10)
    );
  });
});

// ── WASM init ─────────────────────────────────────────────────────────────────

if (window._wasmLoadFailed) {
  console.warn('engine.js failed to load — benchmark will show error if WASM mode is used');
} else if (typeof Module !== 'undefined') {
  Module.onRuntimeInitialized = function () {
    benchEngineReady = true;
  };
}

// ── Histogram ─────────────────────────────────────────────────────────────────

function drawHistogram(canvas, samples) {
  if (!samples || samples.length === 0) return;

  const dpr = window.devicePixelRatio || 1;
  const wrap = canvas.parentElement;
  canvas.width        = wrap.clientWidth * dpr;
  canvas.height       = 120 * dpr;
  canvas.style.width  = wrap.clientWidth + 'px';
  canvas.style.height = '120px';

  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  const W = wrap.clientWidth;
  const H = 120;

  ctx.clearRect(0, 0, W, H);

  const min = Math.min(...samples);
  const max = Math.max(...samples);
  const range = Math.max(max - min, 1);

  const NUM_BUCKETS = Math.min(40, samples.length);
  const buckets = new Array(NUM_BUCKETS).fill(0);
  for (const s of samples) {
    const idx = Math.min(NUM_BUCKETS - 1, Math.floor((s - min) / range * NUM_BUCKETS));
    buckets[idx]++;
  }

  const maxCount = Math.max(...buckets, 1);
  const PAD_B = 22, PAD_T = 6, PAD_L = 2, PAD_R = 2;
  const chartH = H - PAD_T - PAD_B;
  const barW   = (W - PAD_L - PAD_R) / NUM_BUCKETS;

  for (let i = 0; i < NUM_BUCKETS; i++) {
    const barH = (buckets[i] / maxCount) * chartH;
    const x = PAD_L + i * barW + 0.5;
    const y = PAD_T + chartH - barH;
    ctx.fillStyle = CHART.BAR;  // F20
    ctx.fillRect(x, y, Math.max(1, barW - 1), barH);
  }

  // X-axis labels
  ctx.fillStyle  = CHART.LABEL;  // F20
  ctx.font       = '9px Courier New, monospace';
  ctx.textAlign  = 'left';
  ctx.fillText(min + ' ns', PAD_L, H - 4);
  ctx.textAlign  = 'center';
  ctx.fillText(Math.round((min + max) / 2) + ' ns', W / 2, H - 4);
  ctx.textAlign  = 'right';
  ctx.fillText(max + ' ns', W - PAD_R, H - 4);

  // Axis line
  ctx.strokeStyle = CHART.GRID;  // F20
  ctx.lineWidth   = 1;
  ctx.beginPath();
  ctx.moveTo(PAD_L, PAD_T + chartH);
  ctx.lineTo(W - PAD_R, PAD_T + chartH);
  ctx.stroke();
}

// ── Run benchmark ─────────────────────────────────────────────────────────────

document.getElementById('bench-run-btn').addEventListener('click', async function () {
  const errEl = document.getElementById('bench-error');
  errEl.style.display = 'none';

  const totalOps  = parseInt(document.getElementById('bench-ops').value, 10);
  const limitPct  = parseInt(document.getElementById('bench-limit-pct').value, 10);
  const cancelPct = parseInt(document.getElementById('bench-cancel-pct').value, 10);
  const marketPct = parseInt(document.getElementById('bench-market-pct').value, 10);

  const maxOps = benchUseNative ? NATIVE_MAX_OPS : WASM_MAX_OPS;  // F21
  if (isNaN(totalOps) || totalOps < 10_000 || totalOps > maxOps) {
    errEl.textContent = `Operations must be between 10,000 and ${maxOps.toLocaleString()}.`;
    errEl.style.display = '';
    return;
  }
  if (isNaN(limitPct) || isNaN(cancelPct) || isNaN(marketPct)) {
    errEl.textContent = 'All percentage fields are required.';
    errEl.style.display = '';
    return;
  }
  if (limitPct + cancelPct + marketPct !== 100) {
    errEl.textContent = `Percentages must sum to 100 (currently ${limitPct + cancelPct + marketPct}).`;
    errEl.style.display = '';
    return;
  }

  const btn = document.getElementById('bench-run-btn');
  btn.textContent = 'RUNNING\u2026';
  btn.disabled    = true;

  // Yield one frame so the button state paints
  await new Promise(r => setTimeout(r, 0));

  let result;
  try {
    if (benchUseNative) {
      const r = await fetch('http://localhost:8765/benchmark', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ops: totalOps, lp: limitPct, cp: cancelPct, mp: marketPct })
      });
      result = await r.json();
    } else {
      if (!benchEngineReady && typeof Module !== 'undefined') {
        // wait briefly for WASM init
        await new Promise(r => {
          const check = setInterval(() => {
            if (benchEngineReady) { clearInterval(check); r(); }
          }, 50);
          setTimeout(() => { clearInterval(check); r(); }, 3000);
        });
      }
      if (!benchEngineReady) {
        errEl.textContent = 'WASM engine not ready. Build engine.js with build_wasm.sh first.';
        errEl.style.display = '';
        btn.textContent = 'RUN BENCHMARK';
        btn.disabled    = false;
        return;
      }
      const raw = Module._wasm_run_benchmark(totalOps, limitPct, cancelPct, marketPct);
      result = JSON.parse(Module.UTF8ToString(raw));
    }
  } catch (e) {
    errEl.textContent = 'Benchmark failed: ' + e.message;
    errEl.style.display = '';
    btn.textContent = 'RUN BENCHMARK';
    btn.disabled    = false;
    return;
  }

  // Determine unit suffix based on mode
  const isNative = result.mode === 'native';
  const unitSuffix = isNative ? 'ns' : 'ns (avg)';

  document.getElementById('bench-throughput').textContent = result.throughput.toLocaleString();
  document.getElementById('bench-p50').textContent  = result.p50_ns.toLocaleString();
  document.getElementById('bench-p99').textContent  = result.p99_ns.toLocaleString();
  document.getElementById('bench-p999').textContent = result.p999_ns.toLocaleString();
  document.getElementById('bench-elapsed').textContent = result.elapsed_ms.toFixed(1);

  document.getElementById('bench-p50-unit').textContent  = unitSuffix;
  document.getElementById('bench-p99-unit').textContent  = unitSuffix;
  document.getElementById('bench-p999-unit').textContent = unitSuffix;

  document.getElementById('bench-results').style.display = '';

  // Draw histogram
  if (result.histogram && result.histogram.length > 0) {
    const histWrap = document.getElementById('bench-histogram-wrap');
    histWrap.style.display = '';
    const canvas = document.getElementById('bench-histogram');
    // Use rAF to ensure wrap is visible before measuring
    requestAnimationFrame(() => drawHistogram(canvas, result.histogram));
  }

  btn.textContent = 'RUN BENCHMARK';
  btn.disabled    = false;
});
