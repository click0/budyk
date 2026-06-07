// SPDX-License-Identifier: BSD-3-Clause
#include "web/spa.h"

#include <cstring>

namespace budyk {

// One-shot HTML/CSS/JS bundle. Edit in place and rebuild — no asset
// pipeline. Style is deliberately minimal so this stays a single file.
const char* const kSpaIndexHtml = R"BUDYK(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>budyk</title>
<style>
:root {
  --bg:#0d1117; --fg:#c9d1d9; --muted:#6e7681; --accent:#58a6ff;
  --warn:#f0a04b; --crit:#ff5252; --bar:#21262d; --bar-fill:#2ea043;
}
* { box-sizing: border-box; }
body { margin:0; padding:1.2rem 1.5rem; font-family: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, monospace;
       background: var(--bg); color: var(--fg); }
h1 { margin:0 0 1rem 0; font-size: 1.1rem; font-weight: 600; }
h1 .host { color: var(--muted); margin-left: .6rem; font-weight: 400; }
.panel { display:flex; flex-direction:column; gap:.55rem; max-width: 64rem; }
.row   { display:grid; grid-template-columns: 7rem 1fr 14rem; gap:.7rem; align-items:center; }
.label { color: var(--muted); }
.value { font-variant-numeric: tabular-nums; text-align: right; color: var(--fg); }
.bar   { height: .9rem; background: var(--bar); border-radius: .25rem; overflow: hidden; }
.bar > i { display:block; height:100%; background: var(--bar-fill); transition: width .3s ease; }
.bar.warn > i { background: var(--warn); }
.bar.crit > i { background: var(--crit); }
.muted { color: var(--muted); }
.right { text-align: right; }
#login { max-width: 22rem; margin: 4rem auto; display: flex; flex-direction: column; gap:.5rem; }
#login input { background: var(--bar); color: var(--fg); border: 1px solid var(--muted); border-radius:.25rem; padding:.5rem .7rem; font: inherit; }
#login button { background: var(--accent); color: #0d1117; border: 0; border-radius:.25rem; padding:.5rem .7rem; font: inherit; cursor: pointer; }
.err { color: var(--crit); font-size: .85rem; }
.hidden { display: none !important; }
.status { color: var(--muted); margin-top: 1rem; font-size: .85rem; }
.hist { max-width: 64rem; margin-top: 1.6rem; }
.hist h2 { font-size: .95rem; font-weight: 600; margin: 0 0 .6rem 0; }
.hist .ctl { display:flex; flex-wrap:wrap; gap:.35rem; align-items:center; margin-bottom:.6rem; }
.hist .ctl .sep { flex: 1; }
.hist button { background: var(--bar); color: var(--fg); border: 1px solid transparent;
               border-radius:.25rem; padding:.3rem .6rem; font: inherit; font-size:.82rem; cursor: pointer; }
.hist button:hover { border-color: var(--muted); }
.hist button.on { background: var(--accent); color: #0d1117; }
.hist .chartwrap { background: var(--bar); border-radius: .35rem; padding:.4rem; }
.hist svg { width:100%; height:auto; display:block; }
.hist .axis { fill: var(--muted); font-size: 13px; }
.hist .line { fill: none; stroke: var(--accent); stroke-width: 2; stroke-linejoin: round; }
.hist .area { fill: var(--accent); opacity: .08; }
</style>
</head>
<body>

<div id="login" class="hidden">
  <h1>budyk &mdash; sign in</h1>
  <input id="pw" type="password" placeholder="password" autofocus>
  <button onclick="doLogin()">Sign in</button>
  <div id="loginErr" class="err"></div>
</div>

<div id="dash" class="hidden">
  <h1>budyk <span class="host" id="host"></span></h1>
  <div class="panel">
    <div class="row"><span class="label">CPU</span>      <div class="bar"><i id="cpuBar"></i></div>     <span class="value" id="cpu">--</span></div>
    <div class="row"><span class="label">Memory</span>   <div class="bar"><i id="memBar"></i></div>     <span class="value" id="mem">--</span></div>
    <div class="row"><span class="label">Swap</span>     <div class="bar"><i id="swapBar"></i></div>    <span class="value" id="swap">--</span></div>
    <div class="row"><span class="label">Load 1m</span>  <div class="bar"><i id="loadBar"></i></div>    <span class="value" id="load">--</span></div>
    <div class="row"><span class="label">Disk</span>     <span class="muted" id="disk">--</span>        <span class="value">&nbsp;</span></div>
    <div class="row"><span class="label">Network</span>  <span class="muted" id="net">--</span>         <span class="value">&nbsp;</span></div>
    <div class="row"><span class="label">Processes</span><span class="muted" id="proc">--</span>        <span class="value">&nbsp;</span></div>
    <div class="row" id="entropyRow"><span class="label">Entropy</span><span class="muted" id="entropy">--</span> <span class="value">&nbsp;</span></div>
    <div class="row" id="thermalRow"><span class="label">Thermal</span><span class="muted" id="thermal">--</span> <span class="value">&nbsp;</span></div>
    <div class="row" id="fileWatchRow"><span class="label">Files</span><span class="muted" id="fileWatch">--</span> <span class="value">&nbsp;</span></div>
    <div class="row"><span class="label">budyk RSS</span><span class="muted" id="self">--</span>        <span class="value">&nbsp;</span></div>
    <div class="row"><span class="label">Uptime</span>   <span class="muted" id="uptime">--</span>      <span class="value">&nbsp;</span></div>
  </div>
  <div class="status" id="status">connecting&hellip;</div>

  <div class="hist">
    <h2>History</h2>
    <div class="ctl" id="metricCtl"></div>
    <div class="ctl">
      <span id="rangeCtl"></span>
      <span class="sep"></span>
      <span class="muted" id="histInfo" style="font-size:.8rem"></span>
    </div>
    <div class="chartwrap">
      <svg id="histSvg" viewBox="0 0 1000 220" preserveAspectRatio="none"
           role="img" aria-label="metric history chart"></svg>
    </div>
  </div>
</div>

<script>
"use strict";

const $ = (id) => document.getElementById(id);

// Unicode sparkline — eight glyph height steps. Values are scaled
// to the per-window max (min 1) so a flat-zero window stays a thin
// baseline and a single spike pegs to the top. Plenty good for the
// 0/1-mostly file_watch signal.
const SPARK_TICKS = "▁▂▃▄▅▆▇█";
function spark(arr) {
  if (!arr.length) return "";
  const m = Math.max(1, ...arr);
  return arr.map(v => SPARK_TICKS[Math.min(7, Math.floor(v / m * 7))]).join("");
}

// Rolling window of recent file-watch event counts. 30 ticks at the
// default L1 cadence (5 min) is ~2.5 h of history — visible at a
// glance and small enough to never feel laggy.
const FW_HIST_MAX = 30;
let fileWatchHist = [];

function fmtBytes(b) {
  if (b == null) return "--";
  const u = ["B","KiB","MiB","GiB","TiB"];
  let i = 0;
  while (b >= 1024 && i < u.length - 1) { b /= 1024; ++i; }
  return (i === 0 ? b.toFixed(0) : b.toFixed(1)) + " " + u[i];
}

function fmtUptime(s) {
  s = Math.max(0, Math.floor(s));
  const d = Math.floor(s / 86400); s -= d * 86400;
  const h = Math.floor(s / 3600);  s -= h * 3600;
  const m = Math.floor(s / 60);    s -= m * 60;
  if (d) return `${d}d ${h}h ${m}m`;
  if (h) return `${h}h ${m}m ${s}s`;
  return `${m}m ${s}s`;
}

function setBar(id, pct, warnAt = 70, critAt = 90) {
  const el = $(id);
  if (!el) return;
  pct = Math.max(0, Math.min(100, pct));
  el.style.width = pct.toFixed(1) + "%";
  el.parentElement.classList.toggle("warn", pct >= warnAt && pct < critAt);
  el.parentElement.classList.toggle("crit", pct >= critAt);
}

function render(s) {
  if (!s) return;
  $("cpu").textContent = (s.cpu.total_percent ?? 0).toFixed(1) + "% / " + (s.cpu.count ?? 0) + " cores";
  setBar("cpuBar", s.cpu.total_percent ?? 0);

  const memUsedPct = 100 - (s.mem.available_percent ?? 0);
  $("mem").textContent = `${fmtBytes(s.mem.available)} free / ${fmtBytes(s.mem.total)}`;
  setBar("memBar", memUsedPct);

  $("swap").textContent = (s.swap.used_percent ?? 0).toFixed(1) + "%";
  setBar("swapBar", s.swap.used_percent ?? 0, 50, 80);

  const cores = Math.max(1, s.cpu.count ?? 1);
  const loadPct = ((s.load.avg_1m ?? 0) / (cores * 1.5)) * 100;
  $("load").textContent = `${(s.load.avg_1m ?? 0).toFixed(2)} / ${(s.load.avg_5m ?? 0).toFixed(2)} / ${(s.load.avg_15m ?? 0).toFixed(2)}`;
  setBar("loadBar", loadPct);

  $("disk").textContent =
    `r ${fmtBytes(s.disk.read_bytes_per_sec)}/s   w ${fmtBytes(s.disk.write_bytes_per_sec)}/s   (${s.disk.device_count ?? 0} devs)`;
  $("net").textContent =
    `rx ${fmtBytes(s.net.rx_bytes_per_sec)}/s   tx ${fmtBytes(s.net.tx_bytes_per_sec)}/s   (${s.net.interface_count ?? 0} ifaces)`;
  $("proc").textContent =
    `${s.proc?.running ?? 0} running / ${s.proc?.total ?? 0} total`;

  // Entropy is Linux-only; hide the row on platforms (FreeBSD) that
  // report present=false.
  if (s.entropy?.present) {
    $("entropyRow").classList.remove("hidden");
    $("entropy").textContent = `${s.entropy.available_bits} bits in pool`;
  } else {
    $("entropyRow").classList.add("hidden");
  }

  // Thermal — hide the row on hosts without ACPI / sensor passthrough.
  if (s.thermal?.present) {
    $("thermalRow").classList.remove("hidden");
    $("thermal").textContent =
      `${s.thermal.max_celsius.toFixed(1)} °C across ${s.thermal.sensor_count} sensor(s)`;
  } else {
    $("thermalRow").classList.add("hidden");
  }

  // File watcher — hide the row unless security.file_watch is enabled.
  // Shows how many watched paths changed this tick (a spike means
  // tampering just happened) out of the configured total, plus a
  // unicode sparkline of the last FW_HIST_MAX ticks so the operator
  // can see *when* changes happened, not only the current tick.
  if (s.file_watch?.present) {
    $("fileWatchRow").classList.remove("hidden");
    const ev = s.file_watch.events_this_tick ?? 0;
    const wc = s.file_watch.watched_count ?? 0;
    fileWatchHist.push(ev);
    if (fileWatchHist.length > FW_HIST_MAX) fileWatchHist.shift();
    const sp = spark(fileWatchHist);
    $("fileWatch").textContent = ev > 0
      ? `${ev} change(s) this tick across ${wc} watched   ${sp}`
      : `quiet — ${wc} watched   ${sp}`;
  } else {
    $("fileWatchRow").classList.add("hidden");
    fileWatchHist = [];
  }

  // Self-metrics — daemon's own footprint.
  if (s.self) {
    const rss  = s.self.rss_bytes ?? 0;
    const peak = s.self.peak_rss_bytes ?? 0;
    const cuser = s.self.cpu_user_seconds ?? 0;
    const csys  = s.self.cpu_system_seconds ?? 0;
    $("self").textContent =
      `RSS ${fmtBytes(rss)} (peak ${fmtBytes(peak)}) — CPU ${cuser.toFixed(2)}s user / ${csys.toFixed(2)}s sys`;
  }

  $("uptime").textContent = fmtUptime(s.uptime_seconds ?? 0);
}

// --- History chart (consumes /api/range) -----------------------------------
// Each metric pulls a scalar from a sample and formats its axis labels.
const METRICS = {
  cpu:  { label: "CPU %",   pick: s => s.cpu?.total_percent ?? 0,
          fmt: v => v.toFixed(0) + "%" },
  mem:  { label: "Mem %",   pick: s => 100 - (s.mem?.available_percent ?? 0),
          fmt: v => v.toFixed(0) + "%" },
  load: { label: "Load 1m", pick: s => s.load?.avg_1m ?? 0,
          fmt: v => v.toFixed(2) },
  disk: { label: "Disk B/s", pick: s => (s.disk?.read_bytes_per_sec ?? 0) +
                                         (s.disk?.write_bytes_per_sec ?? 0),
          fmt: v => fmtBytes(v) + "/s" },
  net:  { label: "Net B/s",  pick: s => (s.net?.rx_bytes_per_sec ?? 0) +
                                         (s.net?.tx_bytes_per_sec ?? 0),
          fmt: v => fmtBytes(v) + "/s" },
};
// Range presets → window in ms + the tier to query first. Short windows
// prefer raw L3 (tier 1); longer ones use the 5-min L1 ring (tier 3),
// which has continuous coverage. loadHistory falls back to the other
// tier when the first returns nothing.
const RANGES = {
  "1h":  { ms: 3600e3,    tier: 1 },
  "6h":  { ms: 21600e3,   tier: 3 },
  "24h": { ms: 86400e3,   tier: 3 },
  "7d":  { ms: 604800e3,  tier: 3 },
};
let histMetric = "cpu";
let histRange  = "6h";

function fmtClock(ms) {
  const d = new Date(ms);
  const p = n => String(n).padStart(2, "0");
  return `${p(d.getHours())}:${p(d.getMinutes())}`;
}
function fmtClockDay(ms) {
  const d = new Date(ms);
  const p = n => String(n).padStart(2, "0");
  return `${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
}

function drawChart(samples) {
  const svg = $("histSvg");
  const m   = METRICS[histMetric];
  const W = 1000, H = 220, padL = 6, padR = 6, padT = 14, padB = 22;

  const pts = (samples || [])
    .map(s => ({ t: Number(s.ts) / 1e6, v: m.pick(s) }))
    .filter(p => Number.isFinite(p.t) && Number.isFinite(p.v));

  if (pts.length < 2) {
    svg.innerHTML =
      '<text x="500" y="110" class="axis" text-anchor="middle">' +
      (pts.length === 0 ? "no data in this range" : "not enough data yet") +
      "</text>";
    return;
  }

  const t0 = pts[0].t, t1 = pts[pts.length - 1].t;
  let vmin = Infinity, vmax = -Infinity;
  for (const p of pts) { if (p.v < vmin) vmin = p.v; if (p.v > vmax) vmax = p.v; }
  // Pad the value axis a touch and never collapse to a zero-height band.
  if (vmax === vmin) { vmax = vmin + 1; }
  const span = vmax - vmin;
  vmin = Math.max(0, vmin - span * 0.08);
  vmax = vmax + span * 0.08;

  const X = t => padL + (t - t0) / ((t1 - t0) || 1) * (W - padL - padR);
  const Y = v => padT + (1 - (v - vmin) / ((vmax - vmin) || 1)) * (H - padT - padB);

  let line = "", area = "";
  for (let i = 0; i < pts.length; ++i) {
    const x = X(pts[i].t).toFixed(1), y = Y(pts[i].v).toFixed(1);
    line += (i === 0 ? "M" : "L") + x + " " + y + " ";
  }
  area = line + "L" + X(t1).toFixed(1) + " " + (H - padB) + " "
              + "L" + X(t0).toFixed(1) + " " + (H - padB) + " Z";

  const longSpan = (t1 - t0) > 86400e3;        // >1 day → show dates
  const tfmt = longSpan ? fmtClockDay : fmtClock;
  const esc = s => s.replace(/&/g, "&amp;").replace(/</g, "&lt;");

  svg.innerHTML =
    `<path class="area" d="${area}"/>` +
    `<path class="line" d="${line.trim()}"/>` +
    `<text x="${padL}" y="11" class="axis">${esc(m.fmt(vmax))}</text>` +
    `<text x="${padL}" y="${H - padB + 16}" class="axis">${esc(m.fmt(vmin))}</text>` +
    `<text x="${padL}" y="${H - 4}" class="axis">${tfmt(t0)}</text>` +
    `<text x="${W - padR}" y="${H - 4}" class="axis" text-anchor="end">${tfmt(t1)}</text>`;
}

async function fetchRange(tier, sinceNs) {
  const url = `/api/range?tier=${tier}&since=${sinceNs}&limit=5000`;
  const r = await fetch(url, { credentials: "same-origin" });
  if (!r.ok) throw new Error("range " + r.status);
  const doc = await r.json();
  return doc.samples || [];
}

let histTimer = null;
async function loadHistory() {
  const r = RANGES[histRange];
  if (!r) return;
  // Exact nanosecond `since` via BigInt — Date.now() in ms is well within
  // safe-integer range, the *1e6 product is not, so don't use Number.
  const sinceNs = (BigInt(Date.now()) - BigInt(r.ms)) * 1000000n;
  $("histInfo").textContent = "loading…";
  try {
    let samples = await fetchRange(r.tier, sinceNs.toString());
    let usedTier = r.tier;
    if (samples.length === 0) {                 // fall back to the other ring
      const alt = r.tier === 1 ? 3 : 1;
      const more = await fetchRange(alt, sinceNs.toString());
      if (more.length) { samples = more; usedTier = alt; }
    }
    drawChart(samples);
    const tierName = { 1: "raw", 2: "1-min", 3: "5-min" }[usedTier] || "";
    $("histInfo").textContent = samples.length
      ? `${samples.length} pts · ${tierName}` : "no data";
  } catch (e) {
    $("histInfo").textContent = "error";
    drawChart([]);
  }
}

function wireHistoryControls() {
  const mc = $("metricCtl");
  mc.innerHTML = "";
  for (const [key, def] of Object.entries(METRICS)) {
    const b = document.createElement("button");
    b.textContent = def.label;
    b.className = key === histMetric ? "on" : "";
    b.onclick = () => {
      histMetric = key;
      for (const c of mc.children) c.className = "";
      b.className = "on";
      loadHistory();
    };
    mc.appendChild(b);
  }
  const rc = $("rangeCtl");
  rc.innerHTML = "";
  for (const key of Object.keys(RANGES)) {
    const b = document.createElement("button");
    b.textContent = key;
    b.className = key === histRange ? "on" : "";
    b.onclick = () => {
      histRange = key;
      for (const c of rc.children) c.className = "";
      b.className = "on";
      loadHistory();
    };
    rc.appendChild(b);
  }
  // Refresh the visible window every 60s so it tracks "now" without the
  // operator clicking. The live panel still updates per-tick over WS.
  if (histTimer) clearInterval(histTimer);
  histTimer = setInterval(loadHistory, 60000);
}

function showDash()   { $("login").classList.add("hidden");  $("dash").classList.remove("hidden"); }
function showLogin()  { $("dash").classList.add("hidden");   $("login").classList.remove("hidden"); $("pw").focus(); }
function setStatus(t) { $("status").textContent = t; }

async function doLogin() {
  $("loginErr").textContent = "";
  try {
    const r = await fetch("/api/auth/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: $("pw").value }),
    });
    if (r.ok)             return start();
    if (r.status === 401) $("loginErr").textContent = "Invalid password";
    else                  $("loginErr").textContent = "Server returned " + r.status;
  } catch (e) {
    $("loginErr").textContent = "Network error";
  }
}

let ws = null;
function openWs() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  setStatus("connecting WebSocket…");
  ws = new WebSocket(`${proto}//${location.host}/api/ws`);
  ws.onopen    = () => setStatus("live (WebSocket)");
  ws.onmessage = (e) => {
    try {
      const doc = JSON.parse(e.data);
      const arr = doc.samples || [];
      if (arr.length) render(arr[arr.length - 1]);
    } catch (_) { /* ignore parse errors */ }
  };
  ws.onclose   = () => { setStatus("disconnected, retrying in 2s…"); setTimeout(openWs, 2000); };
  ws.onerror   = () => { try { ws.close(); } catch (_) {} };
}

async function start() {
  $("host").textContent = location.host;
  // Probe auth via /api/samples so the path also covers token expiry.
  const r = await fetch("/api/samples", { credentials: "same-origin" });
  if (r.status === 401) return showLogin();
  if (!r.ok) {
    $("loginErr").textContent = "Server returned " + r.status;
    return showLogin();
  }
  showDash();
  // Wire + populate the history chart (independent of the live WS feed).
  wireHistoryControls();
  loadHistory();
  // Render the catch-up snapshot first, then go live. Backfill the
  // file-watch sparkline from the same snapshot so a page reload
  // doesn't reset the timeline visible to the operator.
  try {
    const doc = await r.json();
    const arr = doc.samples || [];
    const tail = arr.slice(-FW_HIST_MAX);
    for (const s of tail) {
      if (s.file_watch?.present) {
        fileWatchHist.push(s.file_watch.events_this_tick ?? 0);
      }
    }
    if (fileWatchHist.length > FW_HIST_MAX) {
      fileWatchHist = fileWatchHist.slice(-FW_HIST_MAX);
    }
    // render() will push the latest sample's value again, so trim one.
    if (fileWatchHist.length > 0) fileWatchHist.pop();
    if (arr.length) render(arr[arr.length - 1]);
  } catch (_) {}
  openWs();
}

document.addEventListener("DOMContentLoaded", start);
$("pw")?.addEventListener("keydown", (e) => { if (e.key === "Enter") doLogin(); });
</script>
</body>
</html>
)BUDYK";

const size_t kSpaIndexHtmlLen = std::strlen(kSpaIndexHtml);

} // namespace budyk
