// SPDX-License-Identifier: BSD-3-Clause
#include "web/json.h"

#include "core/json_text.h"
#include "core/metric_set.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

namespace budyk {

namespace {

// Append a number to `out`. Doubles are formatted with %.6g (round-trip
// for the precision metrics carry); NaN / Inf collapse to 0 because
// JSON has no representation for them and the daemon never produces
// them in well-formed metric paths anyway.
void append_uint(std::string* out, uint64_t v) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%llu",
                          static_cast<unsigned long long>(v));
    if (n > 0) out->append(buf, static_cast<size_t>(n));
}
void append_int(std::string* out, int64_t v) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%lld",
                          static_cast<long long>(v));
    if (n > 0) out->append(buf, static_cast<size_t>(n));
}
void append_double(std::string* out, double v) {
    if (!std::isfinite(v)) v = 0.0;
    char buf[40];
    int n = std::snprintf(buf, sizeof(buf), "%.6g", v);
    if (n > 0) out->append(buf, static_cast<size_t>(n));
}

void append_kv_double(std::string* out, const char* key, double v) {
    out->push_back('"'); out->append(key); out->append("\":");
    append_double(out, v);
}
void append_kv_uint(std::string* out, const char* key, uint64_t v) {
    out->push_back('"'); out->append(key); out->append("\":");
    append_uint(out, v);
}
void append_kv_int(std::string* out, const char* key, int64_t v) {
    out->push_back('"'); out->append(key); out->append("\":");
    append_int(out, v);
}
void append_kv_str(std::string* out, const char* key, const char* v) {
    out->push_back('"'); out->append(key); out->append("\":\"");
    out->append(json_escape(v));
    out->push_back('"');
}

// "key":["name",...], — the name of each bit of `bits` up to `last`.
void append_names(std::string* out, const char* key, uint32_t bits,
                  const char* (*name_of)(uint32_t), uint32_t last) {
    out->push_back('"'); out->append(key); out->append("\":[");
    bool first = true;
    for (uint32_t bit = 1; bit <= last; bit <<= 1) {
        const char* name = (bits & bit) != 0 ? name_of(bit) : nullptr;
        if (name == nullptr) continue;
        if (!first) out->push_back(',');
        first = false;
        out->push_back('"'); out->append(name); out->push_back('"');
    }
    out->append("],");
}

} // namespace

std::string sample_to_json(const Sample& s) {
    std::string out;
    out.reserve(1024);
    out.push_back('{');

    append_kv_uint(&out, "ts",    s.timestamp_nanos);                 out.push_back(',');
    append_kv_int (&out, "level", static_cast<int64_t>(s.level));     out.push_back(',');
    // Only on a record folded from finer samples (/api/range); live
    // samples never carry it.
    if (s.folded) out.append("\"folded\":true,");
    // The groups this sample has; the others are zeros, not readings.
    // "baseline" (live samples only, when there is one): those read for
    // the first time in a row, a rate from the next tick.
    append_names(&out, "groups", s.groups, metric_group_name, kGroupsAll);
    if (s.baseline != 0) append_names(&out, "baseline", s.baseline, metric_group_name, kGroupsAll);

    out.append("\"cpu\":{");
    append_kv_double(&out, "total_percent",  s.cpu.total_percent);    out.push_back(',');
    append_kv_uint  (&out, "count",          s.cpu.count);            out.push_back(',');
    append_kv_double(&out, "user_percent",   s.cpu.user_percent);     out.push_back(',');
    append_kv_double(&out, "system_percent", s.cpu.system_percent);   out.push_back(',');
    append_kv_double(&out, "iowait_percent", s.cpu.iowait_percent);   out.push_back(',');
    append_kv_double(&out, "irq_percent",    s.cpu.irq_percent);      out.push_back(',');
    append_kv_double(&out, "steal_percent",  s.cpu.steal_percent);    out.push_back(',');
    append_kv_double(&out, "idle_percent",   s.cpu.idle_percent);
    out.append("},");

    out.append("\"mem\":{");
    append_kv_uint  (&out, "total",             s.mem.total);             out.push_back(',');
    append_kv_uint  (&out, "available",         s.mem.available);         out.push_back(',');
    append_kv_double(&out, "available_percent", s.mem.available_percent);
    out.append("},");

    out.append("\"swap\":{");
    append_kv_uint  (&out, "total",        s.swap.total);             out.push_back(',');
    append_kv_uint  (&out, "used",         s.swap.used);              out.push_back(',');
    append_kv_double(&out, "used_percent", s.swap.used_percent);
    out.append("},");

    out.append("\"load\":{");
    append_kv_double(&out, "avg_1m",  s.load.avg_1m);                 out.push_back(',');
    append_kv_double(&out, "avg_5m",  s.load.avg_5m);                 out.push_back(',');
    append_kv_double(&out, "avg_15m", s.load.avg_15m);
    out.append("},");

    out.append("\"disk\":{");
    append_kv_uint(&out, "read_bytes_per_sec",  s.disk.read_bytes_per_sec);  out.push_back(',');
    append_kv_uint(&out, "write_bytes_per_sec", s.disk.write_bytes_per_sec); out.push_back(',');
    append_kv_uint(&out, "device_count",        s.disk.device_count);
    out.append("},");

    out.append("\"net\":{");
    append_kv_uint(&out, "rx_bytes_per_sec", s.net.rx_bytes_per_sec); out.push_back(',');
    append_kv_uint(&out, "tx_bytes_per_sec", s.net.tx_bytes_per_sec); out.push_back(',');
    append_kv_uint(&out, "interface_count",  s.net.interface_count);
    out.append("},");

    out.append("\"proc\":{");
    append_kv_uint(&out, "total",    s.proc.total);    out.push_back(',');
    append_kv_uint(&out, "running",  s.proc.running);  out.push_back(',');
    append_kv_uint(&out, "sleeping", s.proc.sleeping); out.push_back(',');
    append_kv_uint(&out, "blocked",  s.proc.blocked);  out.push_back(',');
    append_kv_uint(&out, "stopped",  s.proc.stopped);  out.push_back(',');
    append_kv_uint(&out, "zombie",   s.proc.zombie);   out.push_back(',');
    append_kv_uint(&out, "threads",  s.proc.threads);
    out.append("},");

    out.append("\"entropy\":{");
    append_kv_uint(&out, "available_bits", s.entropy.available_bits);
    out.push_back(',');
    out.append("\"present\":");
    out.append(s.entropy.present ? "true" : "false");
    out.append("},");

    out.append("\"self\":{");
    append_kv_uint  (&out, "rss_bytes",          s.self_.rss_bytes);          out.push_back(',');
    append_kv_uint  (&out, "peak_rss_bytes",     s.self_.peak_rss_bytes);     out.push_back(',');
    append_kv_double(&out, "cpu_user_seconds",   s.self_.cpu_user_seconds);   out.push_back(',');
    append_kv_double(&out, "cpu_system_seconds", s.self_.cpu_system_seconds);
    out.append("},");

    out.append("\"thermal\":{");
    append_kv_double(&out, "max_celsius",  s.thermal.max_celsius); out.push_back(',');
    append_kv_uint  (&out, "sensor_count", s.thermal.sensor_count); out.push_back(',');
    out.append("\"present\":");
    out.append(s.thermal.present ? "true" : "false");
    out.append("},");

    out.append("\"file_watch\":{");
    append_kv_uint(&out, "events_this_tick", s.file_watch.events_this_tick);
    out.push_back(',');
    append_kv_uint(&out, "watched_count",    s.file_watch.watched_count);
    out.push_back(',');
    out.append("\"present\":");
    out.append(s.file_watch.present ? "true" : "false");
    out.append("},");

    out.append("\"tcp\":{");
    append_kv_uint  (&out, "established",           s.tcp.established);           out.push_back(',');
    append_kv_uint  (&out, "time_wait",             s.tcp.time_wait);             out.push_back(',');
    append_kv_double(&out, "active_opens_per_sec",  s.tcp.active_opens_per_sec);  out.push_back(',');
    append_kv_double(&out, "passive_opens_per_sec", s.tcp.passive_opens_per_sec); out.push_back(',');
    append_kv_double(&out, "retransmits_per_sec",   s.tcp.retransmits_per_sec);   out.push_back(',');
    append_kv_double(&out, "in_segments_per_sec",   s.tcp.in_segments_per_sec);   out.push_back(',');
    append_kv_double(&out, "out_segments_per_sec",  s.tcp.out_segments_per_sec);  out.push_back(',');
    append_kv_double(&out, "in_errors_per_sec",     s.tcp.in_errors_per_sec);     out.push_back(',');
    out.append("\"present\":");
    out.append(s.tcp.present ? "true" : "false");
    out.append("},");

    out.append("\"udp\":{");
    append_kv_double(&out, "in_datagrams_per_sec",  s.udp.in_datagrams_per_sec);  out.push_back(',');
    append_kv_double(&out, "out_datagrams_per_sec", s.udp.out_datagrams_per_sec); out.push_back(',');
    append_kv_double(&out, "no_port_per_sec",       s.udp.no_port_per_sec);       out.push_back(',');
    append_kv_double(&out, "in_errors_per_sec",     s.udp.in_errors_per_sec);     out.push_back(',');
    out.append("\"present\":");
    out.append(s.udp.present ? "true" : "false");
    out.append("},");

    out.append("\"fs\":{");
    append_kv_uint  (&out, "count",                   s.fs.count);                   out.push_back(',');
    append_kv_double(&out, "max_used_percent",        s.fs.max_used_percent);        out.push_back(',');
    append_kv_double(&out, "max_inodes_used_percent", s.fs.max_inodes_used_percent); out.push_back(',');
    out.append("\"present\":");
    out.append(s.fs.present ? "true" : "false");
    out.append("},");

    out.append("\"clock\":{");
    append_kv_double(&out, "offset_ms",    s.clock.offset_ms);    out.push_back(',');
    append_kv_double(&out, "max_error_ms", s.clock.max_error_ms); out.push_back(',');
    out.append("\"synced\":");
    out.append(s.clock.synced ? "true" : "false");
    out.append(",\"present\":");
    out.append(s.clock.present ? "true" : "false");
    out.append("},");

    append_kv_double(&out, "uptime_seconds", s.uptime_seconds);
    out.push_back('}');
    return out;
}

std::string detail_to_json(const budyk_detail_c& d) {
    std::string out;
    out.reserve(256 + d.ncores * 160 + (d.ndisks + d.nifaces) * 200 + d.nmounts * 320);
    out.push_back('{');
    append_names(&out, "collected", d.collected, metric_detail_name, kDetailAll);
    if (d.baseline != 0) append_names(&out, "baseline", d.baseline, metric_detail_name, kDetailAll);
    out.append("\"cores\":[");
    for (uint32_t i = 0; i < d.ncores; ++i) {
        const budyk_core_c& c = d.cores[i];
        if (i > 0) out.push_back(',');
        out.push_back('{');
        append_kv_double(&out, "total_percent",  c.total_percent);  out.push_back(',');
        append_kv_double(&out, "user_percent",   c.user_percent);   out.push_back(',');
        append_kv_double(&out, "system_percent", c.system_percent); out.push_back(',');
        append_kv_double(&out, "iowait_percent", c.iowait_percent); out.push_back(',');
        append_kv_double(&out, "irq_percent",    c.irq_percent);    out.push_back(',');
        append_kv_double(&out, "steal_percent",  c.steal_percent);  out.push_back(',');
        append_kv_double(&out, "idle_percent",   c.idle_percent);
        out.push_back('}');
    }
    out.append("],\"disks\":[");
    for (uint32_t i = 0; i < d.ndisks; ++i) {
        const budyk_disk_dev_c& x = d.disks[i];
        if (i > 0) out.push_back(',');
        out.push_back('{');
        append_kv_str   (&out, "name",                x.name);                out.push_back(',');
        append_kv_uint  (&out, "read_bytes_per_sec",  x.read_bytes_per_sec);  out.push_back(',');
        append_kv_uint  (&out, "write_bytes_per_sec", x.write_bytes_per_sec); out.push_back(',');
        append_kv_double(&out, "reads_per_sec",       x.reads_per_sec);       out.push_back(',');
        append_kv_double(&out, "writes_per_sec",      x.writes_per_sec);      out.push_back(',');
        append_kv_double(&out, "busy_percent",        x.busy_percent);
        out.push_back('}');
    }
    out.append("],\"interfaces\":[");
    for (uint32_t i = 0; i < d.nifaces; ++i) {
        const budyk_iface_c& x = d.ifaces[i];
        if (i > 0) out.push_back(',');
        out.push_back('{');
        append_kv_str   (&out, "name",               x.name);               out.push_back(',');
        append_kv_uint  (&out, "rx_bytes_per_sec",   x.rx_bytes_per_sec);   out.push_back(',');
        append_kv_uint  (&out, "tx_bytes_per_sec",   x.tx_bytes_per_sec);   out.push_back(',');
        append_kv_double(&out, "rx_packets_per_sec", x.rx_packets_per_sec); out.push_back(',');
        append_kv_double(&out, "tx_packets_per_sec", x.tx_packets_per_sec); out.push_back(',');
        append_kv_double(&out, "rx_errors_per_sec",  x.rx_errors_per_sec);  out.push_back(',');
        append_kv_double(&out, "tx_errors_per_sec",  x.tx_errors_per_sec);  out.push_back(',');
        append_kv_double(&out, "rx_drops_per_sec",   x.rx_drops_per_sec);   out.push_back(',');
        out.append(x.up != 0 ? "\"up\":true" : "\"up\":false");
        out.push_back('}');
    }
    out.append("],\"filesystems\":[");
    for (uint32_t i = 0; i < d.nmounts; ++i) {
        const budyk_mount_c& x = d.mounts[i];
        if (i > 0) out.push_back(',');
        out.push_back('{');
        append_kv_str   (&out, "mount",               x.mount);               out.push_back(',');
        append_kv_str   (&out, "type",                x.type);                out.push_back(',');
        append_kv_str   (&out, "device",              x.device);              out.push_back(',');
        append_kv_uint  (&out, "total_bytes",         x.total_bytes);         out.push_back(',');
        append_kv_uint  (&out, "used_bytes",          x.used_bytes);          out.push_back(',');
        append_kv_uint  (&out, "avail_bytes",         x.avail_bytes);         out.push_back(',');
        append_kv_double(&out, "used_percent",        x.used_percent);        out.push_back(',');
        append_kv_uint  (&out, "inodes_total",        x.inodes_total);        out.push_back(',');
        append_kv_uint  (&out, "inodes_used",         x.inodes_used);         out.push_back(',');
        append_kv_double(&out, "inodes_used_percent", x.inodes_used_percent);
        out.push_back('}');
    }
    out.append("]}");
    return out;
}

std::string samples_to_json(const Sample* samples, size_t n, const std::string* detail_json) {
    std::string out;
    out.reserve((n > 0 ? n * 1024 : 64) + (detail_json != nullptr ? detail_json->size() + 16 : 0));
    out.append("{\"count\":");
    append_uint(&out, static_cast<uint64_t>(n));
    out.append(",\"samples\":[");
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) out.push_back(',');
        if (samples == nullptr) break;
        out.append(sample_to_json(samples[i]));
    }
    out.push_back(']');
    if (detail_json != nullptr && !detail_json->empty()) {
        out.append(",\"detail\":");
        out.append(*detail_json);
    }
    out.append("}\n");
    return out;
}

bool json_get_string(const std::string& body, const char* key, std::string* out) {
    if (key == nullptr || out == nullptr) return false;
    std::string needle = "\"";
    needle.append(key);
    needle.append("\"");
    const auto kpos = body.find(needle);
    if (kpos == std::string::npos) return false;
    const auto colon = body.find(':', kpos + needle.size());
    if (colon == std::string::npos) return false;
    // Only whitespace may sit between the colon and the opening quote;
    // anything else means the value isn't a string.
    size_t open = colon + 1;
    while (open < body.size() &&
           (body[open] == ' ' || body[open] == '\t' ||
            body[open] == '\n' || body[open] == '\r')) {
        ++open;
    }
    if (open >= body.size() || body[open] != '"') return false;
    // The closing quote is the first `"` not preceded by an odd run of
    // backslashes.
    size_t close = open + 1;
    while (close < body.size()) {
        if (body[close] == '\\') { close += 2; continue; }
        if (body[close] == '"')  break;
        ++close;
    }
    if (close >= body.size()) return false;
    *out = json_unescape(body.substr(open + 1, close - open - 1));
    return true;
}

} // namespace budyk
