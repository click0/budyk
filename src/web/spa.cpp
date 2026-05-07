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
    <div class="row"><span class="label">budyk RSS</span><span class="muted" id="self">--</span>        <span class="value">&nbsp;</span></div>
    <div class="row"><span class="label">Uptime</span>   <span class="muted" id="uptime">--</span>      <span class="value">&nbsp;</span></div>
  </div>
  <div class="status" id="status">connecting&hellip;</div>
</div>

<script>
"use strict";

const $ = (id) => document.getElementById(id);

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
  // Render the catch-up snapshot first, then go live.
  try {
    const doc = await r.json();
    const arr = doc.samples || [];
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
