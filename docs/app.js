// taut demo: the real library (wasm) on two surfaces.
//   01 FEEL IT   - tt_feel_*: a persistent Session pair per RTO floor, stepped
//                  from the animation loop; your cursor samples are the messages.
//   02 MEASURE   - tt_run: one deterministic 500-message race per floor.

let run = null, feelInit = null, feelSend = null, feelStep = null;
let ready = false;

const $ = (id) => document.getElementById(id);
const fmt = (n) => Math.round(n).toLocaleString('en-US');
const css = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const INK = css('--ink'), OX = css('--ox'), SUB = css('--sub'), FAINT = css('--faint'), HAIR = css('--hair');

const LINK = { delay: 20, jitter: 10 };
let lossPct = 15;
let feelSeed = 1 + Math.floor(Math.random() * 999999);
// First paint is deterministic: a fixed, typical seed (the sweep in
// bench/BENCHMARKS.md style: some seeds favor the floor 6x, a few punish it
// with loss streaks). New seed explores the real distribution.
let raceSeed = 6;

TautModule({ locateFile: (f) => 'demo/' + f }).then((M) => {
  run = M.cwrap('tt_run', 'string',
    ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number']);
  feelInit = M.cwrap('tt_feel_init', 'number', ['number', 'number', 'number', 'number']);
  feelSend = M.cwrap('tt_feel_send', 'string', ['number', 'number']);
  feelStep = M.cwrap('tt_feel_step', 'string', ['number']);
  feelInit(feelSeed, lossPct, LINK.delay, LINK.jitter);
  ready = true;
  $('led-label').textContent = 'engine live · wasm';
  $('hint').textContent = 'move your cursor in here (or use arrow keys)';
  race();
});

// ---------- 01 FEEL IT ----------
const field = $('field');
const ctx = field.getContext('2d');
const hint = $('hint');
const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches;

let target = null, lastSample = 0, lastFrame = 0, lastUser = -1e9;
const SAMPLE_MS = 25;
const chans = [
  { fill: true,  color: OX,  pos: null, trail: [], lastWall: null, wall: {}, lagEl: $('lag-g'), retxEl: $('retx-a') },
  { fill: false, color: SUB, pos: null, trail: [], lastWall: null, wall: {}, lagEl: $('lag-a'), retxEl: $('retx-b') },
];

function resetFeel() {
  if (!ready) return;
  feelInit(feelSeed, lossPct, LINK.delay, LINK.jitter);
  for (const ch of chans) {
    ch.pos = null; ch.trail = []; ch.lastWall = null; ch.wall = {};
    ch.lagEl.textContent = '–'; ch.retxEl.textContent = '0';
  }
}

$('loss').addEventListener('input', () => {
  lossPct = +$('loss').value;
  $('loss-val').textContent = lossPct + '%';
});
$('loss').addEventListener('change', resetFeel);

function fieldSize() {
  const r = field.getBoundingClientRect();
  if (field.width !== Math.round(r.width * devicePixelRatio)) {
    field.width = Math.round(r.width * devicePixelRatio);
    field.height = Math.round(r.height * devicePixelRatio);
  }
}
function pointerTo(e) {
  const r = field.getBoundingClientRect();
  target = { x: (e.clientX - r.left) / r.width, y: (e.clientY - r.top) / r.height };
  lastUser = performance.now();
  if (ready) hint.style.opacity = 0;
}
field.addEventListener('pointermove', pointerTo);
field.addEventListener('pointerdown', pointerTo);
field.addEventListener('keydown', (e) => {
  const step = 0.04;
  if (!target) target = { x: .5, y: .5 };
  if (e.key === 'ArrowLeft') target.x -= step; else if (e.key === 'ArrowRight') target.x += step;
  else if (e.key === 'ArrowUp') target.y -= step; else if (e.key === 'ArrowDown') target.y += step;
  else return;
  target.x = Math.max(0, Math.min(1, target.x)); target.y = Math.max(0, Math.min(1, target.y));
  lastUser = performance.now();
  if (ready) hint.style.opacity = 0;
  e.preventDefault();
});

function applyStep(ch, res, now) {
  for (const [seq, x, y] of res.msgs) {
    ch.pos = { x, y };
    if (ch.wall[seq] !== undefined) { ch.lastWall = ch.wall[seq]; delete ch.wall[seq]; }
    ch.trail.push({ x, y, t: now });
  }
  ch.retxEl.textContent = fmt(res.retx);
}

function tick(now) {
  fieldSize();
  if (ready && !reduced && now - lastUser > 2500) {
    const t = now / 1000;
    target = { x: .5 + .38 * Math.sin(t * .9), y: .5 + .3 * Math.sin(t * 1.5 + 1.2) };
    hint.textContent = 'autopilot: move your cursor to take over';
    hint.style.opacity = .85;
  }
  if (ready && target && now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    const s = JSON.parse(feelSend(target.x, target.y));
    if (s.a >= 0) chans[0].wall[s.a] = now;
    if (s.b >= 0) chans[1].wall[s.b] = now;
  }
  if (ready) {
    const dt = Math.round(Math.min(120, lastFrame ? now - lastFrame : 0));
    lastFrame = now;
    if (dt > 0) {
      const r = JSON.parse(feelStep(dt));
      applyStep(chans[0], r.a, now);
      applyStep(chans[1], r.b, now);
    }
    for (const ch of chans) {
      ch.trail = ch.trail.filter((p) => now - p.t < 900);
      if (ch.lastWall !== null) ch.lagEl.textContent = Math.round(now - ch.lastWall) + 'ms';
    }
  }

  // ---- draw ----
  const W = field.width, H = field.height, s = devicePixelRatio;
  ctx.clearRect(0, 0, W, H);
  ctx.strokeStyle = HAIR; ctx.lineWidth = 1; ctx.globalAlpha = .55;
  const cell = 56 * s;
  for (let gx = cell; gx < W; gx += cell) { ctx.beginPath(); ctx.moveTo(gx, 0); ctx.lineTo(gx, H); ctx.stroke(); }
  for (let gy = cell; gy < H; gy += cell) { ctx.beginPath(); ctx.moveTo(0, gy); ctx.lineTo(W, gy); ctx.stroke(); }
  ctx.globalAlpha = 1;

  if (target) {
    ctx.strokeStyle = INK; ctx.lineWidth = 1.2 * s;
    const tx = target.x * W, ty = target.y * H, rr = 9 * s;
    ctx.beginPath(); ctx.moveTo(tx - rr, ty); ctx.lineTo(tx + rr, ty);
    ctx.moveTo(tx, ty - rr); ctx.lineTo(tx, ty + rr); ctx.stroke();
  }
  for (const ch of chans) {
    if (!reduced && ch.trail.length > 1) {
      ctx.strokeStyle = ch.color; ctx.globalAlpha = .3; ctx.lineWidth = 1.6 * s;
      ctx.setLineDash(ch.fill ? [] : [5 * s, 5 * s]);
      ctx.beginPath();
      ch.trail.forEach((p, i) => { const x = p.x * W, y = p.y * H; i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
      ctx.stroke(); ctx.setLineDash([]); ctx.globalAlpha = 1;
    }
    if (ch.pos) {
      const x = ch.pos.x * W, y = ch.pos.y * H, r = 7 * s;
      ctx.lineWidth = 2 * s;
      if (ch.fill) { ctx.fillStyle = ch.color; ctx.beginPath(); ctx.arc(x, y, r, 0, 7); ctx.fill(); }
      else { ctx.strokeStyle = ch.color; ctx.beginPath(); ctx.arc(x, y, r, 0, 7); ctx.stroke(); }
    }
  }
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);

// ---------- 02 MEASURE IT ----------
function doRun(rtoFloor) {
  return JSON.parse(run(raceSeed, lossPct, LINK.delay, LINK.jitter, 64, rtoFloor, 500, 32));
}

function drawRace(a, b) {
  const W = 840, H = 340, PL = 52, PR = 70, PT = 18, PB = 40;
  const maxX = Math.max(a.virtual_ms, b.virtual_ms);
  const maxY = Math.max(a.delivered, b.delivered, 1);
  const x = (ms) => PL + (W - PL - PR) * ms / maxX;
  const y = (d) => H - PB - (H - PT - PB) * d / maxY;
  const line = (tl) => tl.map(([ms, d], i) => (i ? 'L' : 'M') + x(ms).toFixed(1) + ' ' + y(d).toFixed(1)).join(' ');
  let g = '';
  for (let i = 0; i <= 4; i++) {
    const yy = PT + (H - PT - PB) * i / 4;
    g += `<line x1="${PL}" y1="${yy}" x2="${W - PR}" y2="${yy}" stroke="${HAIR}" stroke-width="1"/>`;
    g += `<text x="${PL - 8}" y="${yy + 4}" text-anchor="end" fill="${FAINT}" font-size="12" font-family="Times New Roman, serif">${fmt(maxY * (4 - i) / 4)}</text>`;
  }
  for (let i = 0; i <= 4; i++) {
    g += `<text x="${PL + (W - PL - PR) * i / 4}" y="${H - 12}" text-anchor="middle" fill="${FAINT}" font-size="12" font-family="Times New Roman, serif">${fmt(maxX * i / 4)}ms</text>`;
  }
  g += `<path d="${line(b.timeline)}" fill="none" stroke="${SUB}" stroke-width="2" stroke-dasharray="6 5"/>`;
  g += `<path d="${line(a.timeline)}" fill="none" stroke="${OX}" stroke-width="2.5"/>`;
  const eA = a.timeline[a.timeline.length - 1] || [0, 0], eB = b.timeline[b.timeline.length - 1] || [0, 0];
  g += `<text x="${x(eA[0]) + 6}" y="${y(eA[1]) + 4}" fill="${OX}" font-size="13" font-style="italic" font-family="Times New Roman, serif">25ms</text>`;
  g += `<text x="${x(eB[0]) + 6}" y="${y(eB[1]) + 4}" fill="${SUB}" font-size="13" font-style="italic" font-family="Times New Roman, serif">200ms</text>`;
  $('race').innerHTML = g;
}

function row(label, a, b) {
  const r = b / Math.max(1, a);
  return `<tr><td>${label}</td><td class="ox">${fmt(a)}</td><td>${fmt(b)}</td><td class="ratio">${r.toFixed(1)}</td></tr>`;
}

function race() {
  if (!ready) return;
  const a = doRun(25);
  const b = doRun(200);
  drawRace(a, b);
  $('seed-label').textContent = `seed ${raceSeed} · loss ${lossPct}% · delay ${LINK.delay}ms ± ${LINK.jitter}`;
  $('tbody').innerHTML =
    row('median latency', a.lat_p50, b.lat_p50) + row('p99 latency', a.lat_p99, b.lat_p99) +
    row('worst message', a.lat_max, b.lat_max) + row('all 500 done in', a.virtual_ms, b.virtual_ms);
  const v = $('verdict');
  if (a.timed_out || b.timed_out || !a.ordered || !b.ordered) {
    v.className = 'verdict bad';
    v.textContent = `A run did not complete in order: that would be a protocol bug. Please open an issue with seed ${raceSeed}.`;
    return;
  }
  v.className = 'verdict';
  const ratio = b.lat_max / Math.max(1, a.lat_max);
  if (ratio >= 1.15) {
    v.innerHTML =
      `All 500 delivered exactly once, in order, on both runs. The 25ms floor finished its worst ` +
      `message <b>${ratio.toFixed(1)}&times; sooner</b>; the gap is dead air the OS default ` +
      `spends waiting. Change the loss, or roll a new seed.`;
  } else if (ratio > 0.87) {
    v.innerHTML =
      `All 500 delivered exactly once, in order, on both runs. This seed was a wash: few losses ` +
      `needed a timeout to repair, so the floor barely mattered. Roll a new seed.`;
  } else {
    v.innerHTML =
      `All 500 delivered exactly once, in order, on both runs, and <b>this seed went against the ` +
      `25ms floor</b>: a streak of consecutive losses (acks cross the same lossy link) hit its run ` +
      `and exponential backoff compounded the damage, while the 200ms run drew luckier weather. ` +
      `That tail is real, so it gets shown. Roll a few seeds and watch the distribution.`;
  }
}
$('run').addEventListener('click', race);
$('reseed').addEventListener('click', () => { raceSeed = 1 + Math.floor(Math.random() * 999999); race(); });
