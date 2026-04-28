// SPDX-License-Identifier: BSD-3-Clause
#include "web/json.h"

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

} // namespace

std::string sample_to_json(const Sample& s) {
    std::string out;
    out.reserve(512);
    out.push_back('{');

    append_kv_uint(&out, "ts",    s.timestamp_nanos);                 out.push_back(',');
    append_kv_int (&out, "level", static_cast<int64_t>(s.level));     out.push_back(',');

    out.append("\"cpu\":{");
    append_kv_double(&out, "total_percent", s.cpu.total_percent);     out.push_back(',');
    append_kv_uint  (&out, "count",         s.cpu.count);
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

    append_kv_double(&out, "uptime_seconds", s.uptime_seconds);
    out.push_back('}');
    return out;
}

std::string samples_to_json(const Sample* samples, size_t n) {
    std::string out;
    out.reserve(n > 0 ? n * 512 : 64);
    out.append("{\"count\":");
    append_uint(&out, static_cast<uint64_t>(n));
    out.append(",\"samples\":[");
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) out.push_back(',');
        if (samples == nullptr) break;
        out.append(sample_to_json(samples[i]));
    }
    out.append("]}\n");
    return out;
}

} // namespace budyk
