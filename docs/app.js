// taut demo: run the real protocol over SimNet's virtual clock, twice per race
// (rto floor 25ms vs 200ms, same seed), then draw the delivery curves.

let run = null;      // cwrap'd tt_run
let seed = 1;
let lastRace = null; // { a, b } parsed results for the tooltip

const $ = (id) => document.getElementById(id);
const fmt = (n) => Number(n).toLocaleString('en-US');

TautModule({ locateFile: (f) => 'demo/' + f }).then((M) => {
  run = M.cwrap('tt_run', 'string',
    ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number']);
  $('led').dataset.state = 'on';
  $('led-label').textContent = 'engine loaded · deterministic simulator';
  $('panel').hidden = false;
  reseed();
});

function reseed() {
  seed = 1 + Math.floor(Math.random() * 999999);
  $('seed-label').textContent = 'seed ' + seed;
}
$('reseed').addEventListener('click', () => { reseed(); race(); });

for (const [id, unit] of [['loss', '%'], ['delay', 'ms'], ['jitter', 'ms'], ['count', '']]) {
  $(id).addEventListener('input', () => { $(id + '-val').textContent = $(id).value + unit; });
}

function doRun(rtoFloor) {
  return JSON.parse(run(
    seed, +$('loss').value, +$('delay').value, +$('jitter').value,
    64, rtoFloor, +$('count').value, 32));
}

function tilesHtml(r) {
  const t = (l, v) => `<div class="tile"><span class="t-label">${l}</span><span class="t-value">${v}</span></div>`;
  return t('p50 latency', fmt(r.lat_p50) + '<small>ms</small>')
       + t('p99 latency', fmt(r.lat_p99) + '<small>ms</small>')
       + t('worst', fmt(r.lat_max) + '<small>ms</small>')
       + t('retransmits', fmt(r.retransmits))
       + t('done in', fmt(r.virtual_ms) + '<small>ms</small>');
}

function race() {
  if (!run) return;
  const a = doRun(25);   // taut's floor
  const b = doRun(200);  // TCP-like floor
  lastRace = { a, b };

  $('tiles-a').innerHTML = tilesHtml(a);
  $('tiles-b').innerHTML = tilesHtml(b);
  drawChart(a, b);

  const n = +$('count').value, loss = $('loss').value;
  const v = $('verdict');
  if (a.timed_out || b.timed_out || !a.ordered || !b.ordered) {
    v.className = 'verdict bad';
    v.textContent = 'something did not complete: this would be a protocol bug, please open an issue with seed ' + seed;
    return;
  }
  const speedup = (b.lat_max / Math.max(1, a.lat_max)).toFixed(1);
  v.className = 'verdict ok';
  v.textContent =
    `all ${fmt(n)} messages delivered exactly once, in order, on both runs, at ${loss}% loss (seed ${seed}).\n` +
    `same code, one knob: the 25ms floor finished its worst message ${speedup}x sooner than the 200ms floor ` +
    `(${fmt(a.lat_max)}ms vs ${fmt(b.lat_max)}ms). that gap is dead air the OS default would have spent waiting.`;
}
$('race').addEventListener('click', race);

// ---------- chart ----------

const W = 720, H = 300, PL = 46, PR = 84, PT = 14, PB = 34;

function drawChart(a, b) {
  const maxX = Math.max(a.virtual_ms, b.virtual_ms);
  const maxY = Math.max(a.delivered, b.delivered);
  const x = (ms) => PL + (W - PL - PR) * ms / maxX;
  const y = (d) => H - PB - (H - PT - PB) * d / maxY;

  const line = (tl) => tl.map(([ms, d], i) => (i ? 'L' : 'M') + x(ms).toFixed(1) + ' ' + y(d).toFixed(1)).join(' ');

  let g = '';
  // grid: 4 horizontal lines + labels
  for (let i = 0; i <= 4; i++) {
    const yy = PT + (H - PT - PB) * i / 4;
    const val = Math.round(maxY * (4 - i) / 4);
    g += `<line class="grid" x1="${PL}" y1="${yy}" x2="${W - PR}" y2="${yy}"/>`;
    g += `<text class="axis-label" x="${PL - 8}" y="${yy + 4}" text-anchor="end">${fmt(val)}</text>`;
  }
  for (let i = 0; i <= 4; i++) {
    const xx = PL + (W - PL - PR) * i / 4;
    g += `<text class="axis-label" x="${xx}" y="${H - 12}" text-anchor="middle">${fmt(Math.round(maxX * i / 4))}ms</text>`;
  }
  g += `<path class="series-b" d="${line(b.timeline)}"/>`;
  g += `<path class="series-a" d="${line(a.timeline)}"/>`;
  // direct labels at line ends
  const endA = a.timeline[a.timeline.length - 1] || [0, 0];
  const endB = b.timeline[b.timeline.length - 1] || [0, 0];
  g += `<text class="end-label" fill="var(--copper-bright)" x="${x(endA[0]) + 6}" y="${y(endA[1]) + 4}">25ms</text>`;
  g += `<text class="end-label" fill="var(--steel)" x="${x(endB[0]) + 6}" y="${y(endB[1]) + 4}">200ms</text>`;
  g += `<g id="hover-layer"></g>`;
  $('chart').innerHTML = g;
}

// hover crosshair + tooltip
$('chart').addEventListener('mousemove', (ev) => {
  if (!lastRace) return;
  const svg = $('chart');
  const rect = svg.getBoundingClientRect();
  const px = (ev.clientX - rect.left) * W / rect.width;
  const maxX = Math.max(lastRace.a.virtual_ms, lastRace.b.virtual_ms);
  const ms = Math.max(0, Math.min(maxX, (px - PL) * maxX / (W - PL - PR)));

  const at = (tl) => {
    let best = tl[0] || [0, 0];
    for (const p of tl) { if (p[0] <= ms) best = p; else break; }
    return best[1];
  };
  const da = at(lastRace.a.timeline), db = at(lastRace.b.timeline);

  const layer = document.getElementById('hover-layer');
  if (layer) {
    const xx = PL + (W - PL - PR) * ms / maxX;
    layer.innerHTML = `<line class="crosshair" x1="${xx}" y1="${PT}" x2="${xx}" y2="${H - PB}"/>`;
  }
  const tip = $('tooltip');
  tip.hidden = false;
  tip.textContent = `t=${fmt(Math.round(ms))}ms\n25ms floor: ${fmt(da)}\n200ms floor: ${fmt(db)}`;
  tip.style.left = Math.min(ev.clientX - rect.left + 14, rect.width - 150) + 'px';
  tip.style.top = (ev.clientY - rect.top + 10) + 'px';
});
$('chart').addEventListener('mouseleave', () => {
  $('tooltip').hidden = true;
  const layer = document.getElementById('hover-layer');
  if (layer) layer.innerHTML = '';
});
