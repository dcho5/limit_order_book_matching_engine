// benchmark.js — Benchmark page controller
'use strict';

let benchEngineReady = false;
let benchUseNative   = false;

// ── Mode detection ────────────────────────────────────────────────────────────

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
    input.min  = 10000;
    input.max  = 35000000;
    document.querySelector('.bench-input-hint').textContent = '10K – 35M';
  } else {
    input.min  = 10000;
    input.max  = 10000000;
    document.querySelector('.bench-input-hint').textContent = '10K – 10M';
  }
}

// Probe native server; fall back to WASM silently
fetch('http://localhost:8765/health', { signal: AbortSignal.timeout(800) })
  .then(r => r.json())
  .then(d => { if (d.ok) { benchUseNative = true; updateModeBadge(); } applyOpsLimits(); })
  .catch(() => { applyOpsLimits(); });

// Show download link
(function () {
  const note = document.getElementById('bench-download-note');
  if (note) {
    note.innerHTML = ' &mdash; <a href="https://github.com/dcho-jaewook/limit-order-book-matching-engine/releases/latest" ' +
      'style="color:var(--yellow)">Download native server</a> for nanosecond-resolution timing (8–13M ops/sec).';
  }
})();

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
    ctx.fillStyle = '#ffd32a99';
    ctx.fillRect(x, y, Math.max(1, barW - 1), barH);
  }

  // X-axis labels
  ctx.fillStyle  = '#5a6a80';
  ctx.font       = '9px Courier New, monospace';
  ctx.textAlign  = 'left';
  ctx.fillText(min + ' ns', PAD_L, H - 4);
  ctx.textAlign  = 'center';
  ctx.fillText(Math.round((min + max) / 2) + ' ns', W / 2, H - 4);
  ctx.textAlign  = 'right';
  ctx.fillText(max + ' ns', W - PAD_R, H - 4);

  // Axis line
  ctx.strokeStyle = '#1e2d45';
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

  const maxOps = benchUseNative ? 35_000_000 : 10_000_000;
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
