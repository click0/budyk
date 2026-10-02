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

    out.append("\"proc\":{");
    append_kv_uint(&out, "total",   s.proc.total);   out.push_back(',');
    append_kv_uint(&out, "running", s.proc.running);
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

std::string json_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i++];
            continue;
        }
        const char c = s[i + 1];
        switch (c) {
            case '"':  out += '"';  i += 2; break;
            case '\\': out += '\\'; i += 2; break;
            case '/':  out += '/';  i += 2; break;
            case 'b':  out += '\b'; i += 2; break;
            case 'f':  out += '\f'; i += 2; break;
            case 'n':  out += '\n'; i += 2; break;
            case 'r':  out += '\r'; i += 2; break;
            case 't':  out += '\t'; i += 2; break;
            case 'u': {
                if (i + 5 >= s.size()) { out += s[i++]; break; }
                unsigned code = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = s[i + 2 + k];
                    code <<= 4;
                    if      (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                    else { code = 0xFFFD; break; }
                }
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                i += 6;
                break;
            }
            default:   out += c;    i += 2; break;
        }
    }
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
