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
<link rel="icon" href="data:,">
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
h1 button { float: right; background: var(--bar); color: var(--fg); border: 1px solid transparent;
            border-radius:.25rem; padding:.2rem .6rem; font: inherit; font-size:.8rem; font-weight: 400; cursor: pointer; }
h1 button:hover { border-color: var(--muted); }
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
.row.off { opacity: .45; }
.alerts { max-width: 64rem; margin-top: 1.2rem; }
.alerts h2 { font-size: .95rem; font-weight: 600; margin: 0 0 .5rem 0; }
.alerts table { width:100%; border-collapse: collapse; font-size:.85rem; }
.alerts td { padding:.2rem .5rem; border-top: 1px solid var(--bar); vertical-align: top; }
.alerts td.when { color: var(--muted); white-space: nowrap; }
.alerts .critical { color: var(--crit); }
.alerts .warning  { color: var(--warn); }
.alerts .info     { color: var(--muted); }
.status { color: var(--muted); margin-top: 1rem; font-size: .85rem; }
.detail { max-width: 64rem; margin-top: 1.6rem; }
.detail h2 { font-size: .95rem; font-weight: 600; margin: 1rem 0 .5rem 0; }
.cores { display:grid; grid-template-columns: repeat(auto-fill, minmax(10rem, 1fr)); gap:.35rem 1.2rem; }
.cores .core { display:grid; grid-template-columns: 1.6rem 1fr 2.6rem; gap:.3rem; align-items:center; font-size:.8rem; }
.detail table { width:100%; border-collapse: collapse; font-size:.85rem; font-variant-numeric: tabular-nums; }
.detail th { color: var(--muted); font-weight: 400; text-align: right; padding:.2rem .5rem; }
.detail td { text-align: right; padding:.2rem .5rem; border-top: 1px solid var(--bar); }
.detail th:first-child, .detail td:first-child { text-align: left; }
.detail td .bar { width: 8rem; display:inline-block; vertical-align: middle; }
.down { color: var(--crit); }
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
  <button id="signIn" onclick="doLogin()">Sign in</button>
  <div id="loginErr" class="err"></div>
</div>

<div id="dash" class="hidden">
  <h1>budyk <span class="host" id="host"></span><button id="signOut" class="hidden" onclick="doLogout()">sign out</button></h1>
  <div class="panel">
    <div class="row"><span class="label">Level</span>    <span class="muted" id="level">--</span>       <span class="value">&nbsp;</span></div>
    <div class="row" data-group="cpu"><span class="label">CPU</span>      <div class="bar"><i id="cpuBar"></i></div>     <span class="value" id="cpu">--</span></div>
    <div class="row" data-group="cpu"><span class="label">CPU time</span> <span class="muted" id="cpuParts">--</span>    <span class="value">&nbsp;</span></div>
    <div class="row" data-group="mem"><span class="label">Memory</span>   <div class="bar"><i id="memBar"></i></div>     <span class="value" id="mem">--</span></div>
    <div class="row" data-group="swap"><span class="label">Swap</span>     <div class="bar"><i id="swapBar"></i></div>    <span class="value" id="swap">--</span></div>
    <div class="row" data-group="load"><span class="label">Load 1m</span>  <div class="bar"><i id="loadBar"></i></div>    <span class="value" id="load">--</span></div>
    <div class="row" data-group="disk"><span class="label">Disk</span>     <span class="muted" id="disk">--</span>        <span class="value">&nbsp;</span></div>
    <div class="row" data-group="net"><span class="label">Network</span>  <span class="muted" id="net">--</span>         <span class="value">&nbsp;</span></div>
    <div class="row" data-group="proc"><span class="label">Processes</span><span class="muted" id="proc">--</span>        <span class="value">&nbsp;</span></div>
    <div class="row" id="tcpRow" data-group="tcp"><span class="label">TCP</span><span class="muted" id="tcp">--</span> <span class="value">&nbsp;</span></div>
    <div class="row" id="udpRow" data-group="udp"><span class="label">UDP</span><span class="muted" id="udp">--</span> <span class="value">&nbsp;</span></div>
    <div class="row" id="icmpRow" data-group="icmp"><span class="label">ICMP</span><span class="muted" id="icmp">--</span> <span class="value">&nbsp;</span></div>
    <div class="row" id="fsRow" data-group="fs"><span class="label">Filesystems</span><div class="bar"><i id="fsBar"></i></div> <span class="value" id="fs">--</span></div>
    <div class="row" id="clockRow" data-group="clock"><span class="label">Clock</span><span class="muted" id="clock">--</span> <span class="value">&nbsp;</span></div>
    <div class="row" id="entropyRow" data-group="entropy"><span class="label">Entropy</span><span class="muted" id="entropy">--</span> <span class="value">&nbsp;</span></div>
    <div class="row" id="thermalRow" data-group="thermal"><span class="label">Thermal</span><span class="muted" id="thermal">--</span> <span class="value">&nbsp;</span></div>
    <div class="row" id="fileWatchRow"><span class="label">Files</span><span class="muted" id="fileWatch">--</span> <span class="value">&nbsp;</span></div>
    <div class="row" data-group="self"><span class="label">budyk RSS</span><span class="muted" id="self">--</span>        <span class="value">&nbsp;</span></div>
    <div class="row" data-group="uptime"><span class="label">Uptime</span>   <span class="muted" id="uptime">--</span>      <span class="value">&nbsp;</span></div>
  </div>
  <div class="status" id="status">connecting&hellip;</div>

  <div class="alerts hidden" id="alertsBox">
    <h2>Alerts</h2>
    <table><tbody id="alerts"></tbody></table>
  </div>

  <div class="detail hidden" id="detail">
    <h2>Cores</h2>
    <div class="cores" id="cores"></div>
    <h2>Disks</h2>
    <table><thead><tr><th>device</th><th>read</th><th>write</th><th>ops/s</th><th>busy</th></tr></thead>
      <tbody id="disks"></tbody></table>
    <h2>Interfaces</h2>
    <table><thead><tr><th>interface</th><th>rx</th><th>tx</th><th>packets/s</th><th>errors/s</th><th>drops/s</th></tr></thead>
      <tbody id="ifaces"></tbody></table>
    <h2>Filesystems</h2>
    <table><thead><tr><th>mount</th><th>type</th><th>used</th><th>size</th><th></th><th>inodes</th></tr></thead>
      <tbody id="mounts"></tbody></table>
    <h2>Processes</h2>
    <table><thead><tr><th>process</th><th>pid</th><th>state</th><th>CPU</th><th>RSS</th><th>threads</th></tr></thead>
      <tbody id="procs"></tbody></table>
  </div>

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
// Level table from /api/levels: id -> { name, interval_ms }. Samples carry
// only the id; custom levels are defined in the daemon's config.
let levels = {};

function fmtLevel(id) {
  const lv = levels[id];
  if (!lv) return id == null ? "--" : "level " + id;
  const s = lv.interval_ms / 1000;
  const every = s >= 60 ? (s / 60) + " min" : s + " s";
  return `${lv.name} (every ${every})`;
}

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

// The server's clock against the browser's (ms), from the newest sample:
// the history window is the server's last hours, not the browser's.
let serverSkewMs = 0;
let knownCores = 1;

function render(s) {
  if (!s) return;
  const tsMs = Number(s.ts) / 1e6;
  if (Number.isFinite(tsMs) && tsMs > 0) serverSkewMs = tsMs - Date.now();
  $("level").textContent = fmtLevel(s.level);
  $("cpu").textContent = (s.cpu.total_percent ?? 0).toFixed(1) + "% / " + (s.cpu.count ?? 0) + " cores";
  setBar("cpuBar", s.cpu.total_percent ?? 0);

  const memUsedPct = 100 - (s.mem.available_percent ?? 0);
  $("mem").textContent = `${fmtBytes(s.mem.available)} free / ${fmtBytes(s.mem.total)}`;
  setBar("memBar", memUsedPct);

  $("swap").textContent = (s.swap.used_percent ?? 0).toFixed(1) + "%";
  setBar("swapBar", s.swap.used_percent ?? 0, 50, 80);

  // The cores of the last sample that has cpu: one without it (a rate's
  // first read) says 0, and the load bar was drawn against one core.
  if (collected(s, "cpu") && s.cpu.count > 0) knownCores = s.cpu.count;
  const loadPct = ((s.load.avg_1m ?? 0) / (knownCores * 1.5)) * 100;
  $("load").textContent = `${(s.load.avg_1m ?? 0).toFixed(2)} / ${(s.load.avg_5m ?? 0).toFixed(2)} / ${(s.load.avg_15m ?? 0).toFixed(2)}`;
  setBar("loadBar", loadPct);

  $("disk").textContent =
    `r ${fmtBytes(s.disk.read_bytes_per_sec)}/s   w ${fmtBytes(s.disk.write_bytes_per_sec)}/s   (${s.disk.device_count ?? 0} devs)`;
  $("net").textContent =
    `rx ${fmtBytes(s.net.rx_bytes_per_sec)}/s   tx ${fmtBytes(s.net.tx_bytes_per_sec)}/s   (${s.net.interface_count ?? 0} ifaces)`;
  $("proc").textContent =
    `${s.proc?.running ?? 0} running / ${s.proc?.total ?? 0} total` +
    `   ${s.proc?.blocked ?? 0} blocked, ${s.proc?.zombie ?? 0} zombie, ${s.proc?.threads ?? 0} threads`;

  const pc = v => (v ?? 0).toFixed(1) + "%";
  $("cpuParts").textContent =
    `user ${pc(s.cpu.user_percent)}   sys ${pc(s.cpu.system_percent)}   ` +
    `iowait ${pc(s.cpu.iowait_percent)}   irq ${pc(s.cpu.irq_percent)}   ` +
    `steal ${pc(s.cpu.steal_percent)}`;

  // The rows below are hidden when the kernel had nothing to report.
  const show = (id, on) => $(id).classList.toggle("hidden", !on);
  const rate = v => (v ?? 0).toFixed(1) + "/s";
  show("tcpRow", s.tcp?.present);
  if (s.tcp?.present) {
    $("tcp").textContent =
      `${s.tcp.established} established, ${s.tcp.time_wait} time-wait   ` +
      `opens ${rate(s.tcp.active_opens_per_sec)} out / ${rate(s.tcp.passive_opens_per_sec)} in   ` +
      `retransmits ${rate(s.tcp.retransmits_per_sec)}`;
  }
  show("udpRow", s.udp?.present);
  if (s.udp?.present) {
    $("udp").textContent =
      `in ${rate(s.udp.in_datagrams_per_sec)}   out ${rate(s.udp.out_datagrams_per_sec)}   ` +
      `no port ${rate(s.udp.no_port_per_sec)}   errors ${rate(s.udp.in_errors_per_sec)}`;
  }
  show("icmpRow", s.icmp?.present);
  if (s.icmp?.present) {
    $("icmp").textContent =
      `in ${rate(s.icmp.in_msgs_per_sec)}   out ${rate(s.icmp.out_msgs_per_sec)}   ` +
      `errors ${rate(s.icmp.in_errors_per_sec)}   unreachable ${rate(s.icmp.in_dest_unreachs_per_sec)}`;
  }
  show("fsRow", s.fs?.present);
  if (s.fs?.present) {
    // The fullest of the local filesystems, by space and by inodes.
    $("fs").textContent = `${pc(s.fs.max_used_percent)} · inodes ${pc(s.fs.max_inodes_used_percent)}`;
    $("fs").title = `the fullest of ${s.fs.count} local filesystem(s)`;
    setBar("fsBar", s.fs.max_used_percent ?? 0, 80, 90);
  }
  show("clockRow", s.clock?.present);
  if (s.clock?.present) {
    $("clock").textContent = (s.clock.synced ? "synchronised" : "NOT synchronised") +
      `   offset ${(s.clock.offset_ms ?? 0).toFixed(3)} ms   max error ${(s.clock.max_error_ms ?? 0).toFixed(1)} ms`;
  }

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

  // A group the sample's level does not collect (collection.<level>.metrics,
  // or a rate on its first read) holds zeros, not readings: say so.
  for (const row of document.querySelectorAll(".panel .row[data-group]")) {
    const off = !collected(s, row.dataset.group);
    const why = firstRead(s.baseline, row.dataset.group)
      ? "measuring…" : "not collected at this level";
    row.classList.toggle("off", off);
    row.title = off ? why : "";
    if (!off) continue;
    row.classList.remove("hidden");
    for (const el of row.querySelectorAll("[id]")) {
      if (el.tagName === "I") el.style.width = "0";
      else el.textContent = el.classList.contains("value") ? "—" : why;
    }
  }
}

// Whether `name` is in a "baseline" list: read for the first time in a
// row, so a rate only from the next tick.
function firstRead(list, name) {
  return Array.isArray(list) && list.includes(name);
}

// Whether sample `s` has group `g` ("cpu", "disk", ...). A sample without
// the list (an older daemon) has them all.
function collected(s, g) {
  return !Array.isArray(s.groups) || s.groups.includes(g);
}

// --- History chart (consumes /api/range) -----------------------------------
// Each metric pulls a scalar from a sample and formats its axis labels; a
// sample without the metric's group leaves a gap in the line.
const METRICS = {
  cpu:  { label: "CPU %",   group: "cpu", pick: s => s.cpu?.total_percent ?? 0,
          fmt: v => v.toFixed(0) + "%" },
  mem:  { label: "Mem %",   group: "mem", pick: s => 100 - (s.mem?.available_percent ?? 0),
          fmt: v => v.toFixed(0) + "%" },
  load: { label: "Load 1m", group: "load", pick: s => s.load?.avg_1m ?? 0,
          fmt: v => v.toFixed(2) },
  disk: { label: "Disk B/s", group: "disk", pick: s => (s.disk?.read_bytes_per_sec ?? 0) +
                                         (s.disk?.write_bytes_per_sec ?? 0),
          fmt: v => fmtBytes(v) + "/s" },
  net:  { label: "Net B/s",  group: "net", pick: s => (s.net?.rx_bytes_per_sec ?? 0) +
                                         (s.net?.tx_bytes_per_sec ?? 0),
          fmt: v => fmtBytes(v) + "/s" },
  tcp:  { label: "TCP estab", group: "tcp", pick: s => s.tcp?.established ?? 0,
          fmt: v => v.toFixed(0) },
  fs:   { label: "FS used %", group: "fs", pick: s => s.fs?.max_used_percent ?? 0,
          fmt: v => v.toFixed(0) + "%" },
};
// Range presets → window in ms. loadHistory reads every level's ring for
// the window (level=all), thinned server-side to fit the chart.
const RANGES = {
  "1h":  { ms: 3600e3 },
  "6h":  { ms: 21600e3 },
  "24h": { ms: 86400e3 },
  "7d":  { ms: 604800e3 },
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

  // null: not collected there (a gap).
  const all = (samples || [])
    .map(s => ({ t: Number(s.ts) / 1e6, v: collected(s, m.group) ? m.pick(s) : null }))
    .filter(p => Number.isFinite(p.t) && (p.v === null || Number.isFinite(p.v)));
  const pts = all.filter(p => p.v !== null);

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

  // One path piece per run of collected points, the area under each.
  let line = "", area = "", run = [];
  const flush = () => {
    if (run.length === 0) return;
    let piece = "";
    run.forEach((p, i) => { piece += (i === 0 ? "M" : "L") + X(p.t).toFixed(1) + " " + Y(p.v).toFixed(1) + " "; });
    line += piece;
    area += piece + "L" + X(run[run.length - 1].t).toFixed(1) + " " + (H - padB) + " "
                  + "L" + X(run[0].t).toFixed(1) + " " + (H - padB) + " Z ";
    run = [];
  };
  for (const p of all) {
    if (p.v === null) flush(); else run.push(p);
  }
  flush();

  const longSpan = (t1 - t0) > 86400e3;        // >1 day → show dates
  const tfmt = longSpan ? fmtClockDay : fmtClock;
  const esc = s => s.replace(/&/g, "&amp;").replace(/</g, "&lt;");

  svg.innerHTML =
    `<path class="area" d="${area.trim()}"/>` +
    `<path class="line" d="${line.trim()}"/>` +
    `<text x="${padL}" y="11" class="axis">${esc(m.fmt(vmax))}</text>` +
    `<text x="${padL}" y="${H - padB + 16}" class="axis">${esc(m.fmt(vmin))}</text>` +
    `<text x="${padL}" y="${H - 4}" class="axis">${tfmt(t0)}</text>` +
    `<text x="${W - padR}" y="${H - 4}" class="axis" text-anchor="end">${tfmt(t1)}</text>`;
}

// Every level's ring merged by time (level=all), thinned server-side to
// 1000 points spread over the whole window: one per unit of the chart's
// width. 5000 full samples were 8 MB a refresh, cut off on a slow link.
async function fetchRange(sinceNs) {
  const url = `api/range?level=all&since=${sinceNs}&limit=1000`;
  const r = await fetch(url, { credentials: "same-origin" });
  if (!r.ok) throw new Error("range " + r.status);
  const doc = await r.json();
  return doc.samples || [];
}

let histTimer = null;
let histSamples = [];     // the last window fetched: a metric drawn from it
async function loadHistory() {
  const r = RANGES[histRange];
  if (!r) return;
  // Exact nanosecond `since` via BigInt — Date.now() in ms is well within
  // safe-integer range, the *1e6 product is not, so don't use Number.
  const nowMs = Math.round(Date.now() + serverSkewMs);
  const sinceNs = (BigInt(nowMs) - BigInt(r.ms)) * 1000000n;
  $("histInfo").textContent = "loading…";
  try {
    histSamples = await fetchRange(sinceNs.toString());
    drawChart(histSamples);
    $("histInfo").textContent = histSamples.length ? `${histSamples.length} pts` : "no data";
  } catch (e) {
    histSamples = [];
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
      drawChart(histSamples);           // the same window: nothing to fetch
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
  // operator clicking, but not in a tab nobody looks at (it fetched the
  // window every minute for days); one coming back is brought up to date.
  // The live panel still updates per-tick over WS.
  if (histTimer) clearInterval(histTimer);
  histTimer = setInterval(() => { if (!document.hidden) loadHistory(); }, 60000);
}
document.addEventListener("visibilitychange", () => { if (!document.hidden && histTimer) loadHistory(); });

function showDash()   { $("login").classList.add("hidden");  $("dash").classList.remove("hidden"); }
function showLogin()  { $("dash").classList.add("hidden");   $("login").classList.remove("hidden"); $("pw").focus(); }
function setStatus(t) { $("status").textContent = t; }

// One login at a time: Enter and a click while the slow Argon2 check ran
// started the dashboard twice, with two WebSockets for good.
let loggingIn = false;
async function doLogin() {
  if (loggingIn) return;
  loggingIn = true;
  $("signIn").disabled = true;
  $("loginErr").textContent = "";
  try {
    const r = await fetch("api/auth/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: $("pw").value }),
    });
    if (r.ok) {
      $("pw").value = "";               // not left in the page for the session
      return await start();
    }
    if (r.status === 401) $("loginErr").textContent = "Invalid password";
    else if (r.status === 429) {
      const wait = r.headers.get("Retry-After");
      $("loginErr").textContent = "Too many attempts" + (wait ? ", try again in " + wait + " s" : "");
    }
    else                  $("loginErr").textContent = "Server returned " + r.status;
  } catch (e) {
    $("loginErr").textContent = "Network error";
  } finally {
    loggingIn = false;
    $("signIn").disabled = false;
  }
}

// Ends the session: the daemon closes its WebSocket (1008) and the page
// goes back to the sign-in form.
async function doLogout() {
  try { await fetch("api/auth/logout", { method: "POST", credentials: "same-origin" }); } catch (_) {}
  closeWs();
  setStatus("signed out");
  showLogin();
}

// Per-entity detail of the newest sample (doc.detail in a WebSocket
// frame or /api/samples). Names come from the host (a mount point can
// hold anything), so they go in as text, never as markup.
function cell(tr, text, cls) {
  const td = document.createElement("td");
  if (text instanceof Node) td.appendChild(text); else td.textContent = text;
  if (cls) td.className = cls;
  tr.appendChild(td);
}
function miniBar(pct, warnAt, critAt) {
  const bar = document.createElement("div");
  bar.className = "bar";
  const i = document.createElement("i");
  bar.appendChild(i);
  pct = Math.max(0, Math.min(100, pct ?? 0));
  i.style.width = pct.toFixed(1) + "%";
  bar.classList.toggle("warn", pct >= warnAt && pct < critAt);
  bar.classList.toggle("crit", pct >= critAt);
  return bar;
}
function fillRows(tbodyId, items, row, off) {
  const tb = $(tbodyId);
  tb.replaceChildren();
  if (off) {
    const tr = document.createElement("tr");
    const td = document.createElement("td");
    td.colSpan = 6;
    td.className = "muted";
    td.textContent = off;
    tr.appendChild(td);
    tb.appendChild(tr);
  }
  for (const it of items || []) {
    const tr = document.createElement("tr");
    row(tr, it);
    tb.appendChild(tr);
  }
}
// A list the level does not collect gets one line saying so (or that
// its rates come with the next tick).
function notCollected(d, name) {
  if (!Array.isArray(d.collected) || d.collected.includes(name)) return "";
  return firstRead(d.baseline, name) ? "measuring…" : "not collected at this level";
}
function renderDetail(d) {
  if (!d) return;
  $("detail").classList.remove("hidden");
  const cores = $("cores");
  cores.replaceChildren();
  if (notCollected(d, "cpu.cores")) {
    const n = document.createElement("span");
    n.className = "muted";
    n.textContent = notCollected(d, "cpu.cores");
    cores.appendChild(n);
  }
  (d.cores || []).forEach((c, i) => {
    const div = document.createElement("div");
    div.className = "core";
    const n = document.createElement("span");
    n.className = "muted";
    n.textContent = String(c.id ?? i);   // the CPU's number: past an offline one, not i
    const v = document.createElement("span");
    v.className = "right";
    v.textContent = (c.total_percent ?? 0).toFixed(0) + "%";
    div.append(n, miniBar(c.total_percent, 70, 90), v);
    div.title = `user ${(c.user_percent ?? 0).toFixed(1)}%  sys ${(c.system_percent ?? 0).toFixed(1)}%  ` +
                `iowait ${(c.iowait_percent ?? 0).toFixed(1)}%  irq ${(c.irq_percent ?? 0).toFixed(1)}%`;
    cores.appendChild(div);
  });
  const rate = v => fmtBytes(v ?? 0) + "/s";
  fillRows("disks", d.disks, (tr, x) => {
    cell(tr, x.name);
    cell(tr, rate(x.read_bytes_per_sec));
    cell(tr, rate(x.write_bytes_per_sec));
    cell(tr, ((x.reads_per_sec ?? 0) + (x.writes_per_sec ?? 0)).toFixed(1));
    cell(tr, (x.busy_percent ?? 0).toFixed(0) + "%");
  }, notCollected(d, "disk.devices"));
  fillRows("ifaces", d.interfaces, (tr, x) => {
    cell(tr, x.name + (x.up ? "" : " (down)"), x.up ? "" : "down");
    cell(tr, rate(x.rx_bytes_per_sec));
    cell(tr, rate(x.tx_bytes_per_sec));
    cell(tr, ((x.rx_packets_per_sec ?? 0) + (x.tx_packets_per_sec ?? 0)).toFixed(1));
    cell(tr, ((x.rx_errors_per_sec ?? 0) + (x.tx_errors_per_sec ?? 0)).toFixed(1));
    cell(tr, (x.rx_drops_per_sec ?? 0).toFixed(1));
  }, notCollected(d, "net.interfaces"));
  fillRows("mounts", d.filesystems, (tr, x) => {
    cell(tr, x.mount);
    cell(tr, x.type, "muted");
    cell(tr, (x.used_percent ?? 0).toFixed(1) + "%");
    cell(tr, fmtBytes(x.total_bytes ?? 0));
    cell(tr, miniBar(x.used_percent, 80, 90));
    cell(tr, (x.inodes_used_percent ?? 0).toFixed(1) + "%");
  }, notCollected(d, "fs.mounts"));
  // The top processes by CPU and by memory; names are the processes' own.
  fillRows("procs", d.processes, (tr, x) => {
    cell(tr, x.name);
    cell(tr, String(x.pid), "muted");
    cell(tr, x.state || "", "muted");
    cell(tr, (x.cpu_percent ?? 0).toFixed(1) + "%");
    cell(tr, fmtBytes(x.rss_bytes ?? 0));
    cell(tr, String(x.threads ?? 0));
  }, notCollected(d, "proc.top"));
}

// Alerts the rules raised: the latest from /api/alerts when the page
// opens, then each as the WebSocket brings it. Newest first; the rule
// names and messages are the rules', so they go in as text.
// Each alert once (by time and rule): the list is fetched again after
// every reconnect, for the alerts raised while the WebSocket was down,
// and after a new sign-in, which listed every alert twice.
const ALERTS_MAX = 20;
let alertList = [];
function addAlerts(arr) {
  if (!Array.isArray(arr) || arr.length === 0) return;
  const key = a => `${a.ts}|${a.rule}`;
  const seen = new Set(alertList.map(key));
  const fresh = arr.filter(a => !seen.has(key(a)));
  if (fresh.length === 0 && alertList.length > 0) return;
  alertList = alertList.concat(fresh)
    .sort((x, y) => Number(x.ts) - Number(y.ts)).slice(-ALERTS_MAX);
  $("alertsBox").classList.remove("hidden");
  const tb = $("alerts");
  tb.replaceChildren();
  const today = new Date().toDateString();
  for (const a of alertList.slice().reverse()) {
    const tr = document.createElement("tr");
    const ms = Number(a.ts) / 1e6;       // the date too, if not today's
    cell(tr, new Date(ms).toDateString() === today ? fmtClock(ms) : fmtClockDay(ms), "when");
    cell(tr, a.severity || "", a.severity || "");
    cell(tr, a.rule || "");
    cell(tr, a.message || "", "muted");
    tb.appendChild(tr);
  }
}

async function fetchAlerts() {
  try {
    const ar = await fetch("api/alerts", { credentials: "same-origin" });
    if (ar.ok) addAlerts((await ar.json()).alerts);
  } catch (_) {}
}

// The URLs are relative to the page: behind a reverse proxy under a path
// (/budyk/ -> budyk's /) "/api/..." went to the proxy's own /api.
let ws = null;
function closeWs() {
  if (!ws) return;
  const old = ws;
  ws = null;
  old.onclose = null;
  try { old.close(); } catch (_) {}
}
function openWs() {
  closeWs();                             // one stream per page, whatever called
  const u = new URL("api/ws", location.href);
  u.protocol = u.protocol === "https:" ? "wss:" : "ws:";
  setStatus("connecting WebSocket…");
  const sock = new WebSocket(u.href);
  ws = sock;
  sock.onopen    = () => { setStatus("live (WebSocket)"); fetchAlerts(); };
  sock.onmessage = (e) => {
    try {
      const doc = JSON.parse(e.data);
      if (doc.alerts) { addAlerts(doc.alerts); return; }
      const arr = doc.samples || [];
      if (arr.length) render(arr[arr.length - 1]);
      renderDetail(doc.detail);
    } catch (_) { /* ignore parse errors */ }
  };
  // 1008: the session ended (logout, expiry) and the daemon dropped the
  // stream. A refused upgrade (401) closes with no code a page can see,
  // so a close asks /api/levels (small, and behind the login too) before
  // retrying: on 401 the page goes back to the login form instead of
  // "retrying" forever.
  sock.onclose   = async (e) => {
    if (sock !== ws) return;             // replaced: its successor carries on
    if (e.code !== 1008) {
      setStatus("disconnected, retrying in 2s…");
      const again = () => { if (sock === ws) openWs(); };
      try {
        const r = await fetch("api/levels", { credentials: "same-origin" });
        if (r.status !== 401) { setTimeout(again, 2000); return; }
      } catch (_) { setTimeout(again, 2000); return; }
    }
    ws = null;
    setStatus("session ended");
    showLogin();
  };
  sock.onerror   = () => { try { sock.close(); } catch (_) {} };
}

async function start() {
  $("host").textContent = location.host;
  // The host's own name, from /api/health (no login needed).
  try {
    const hr = await fetch("api/health");
    const h = hr.ok ? await hr.json() : {};
    if (h.hostname) {
      $("host").textContent = `${h.hostname} · ${location.host}`;
      document.title = `${h.hostname} — budyk`;
    }
    $("signOut").classList.toggle("hidden", h.auth !== true);
  } catch (_) {}
  // Probe auth via /api/samples so the path also covers token expiry.
  const r = await fetch("api/samples?last=1", { credentials: "same-origin" });
  if (r.status === 401) return showLogin();
  if (!r.ok) {
    $("loginErr").textContent = "Server returned " + r.status;
    return showLogin();
  }
  showDash();
  try {
    const lr = await fetch("api/levels", { credentials: "same-origin" });
    if (lr.ok) for (const lv of await lr.json()) levels[lv.id] = lv;
  } catch (_) {}
  await fetchAlerts();
  // Wire the history chart (independent of the live WS feed); it loads
  // once the catch-up sample has given the server's clock.
  wireHistoryControls();
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
    renderDetail(doc.detail);
  } catch (_) {}
  loadHistory();
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
