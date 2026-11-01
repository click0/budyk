// SPDX-License-Identifier: BSD-3-Clause
#include "daemon/router.h"

#include "core/clock.h"
#include "core/metric_set.h"
#include "web/auth.h"
#include "web/http_util.h"
#include "web/json.h"
#include "web/spa.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace budyk {

namespace {

// The first entry of a header a chain of proxies may have appended to,
// without the blanks around it.
std::string first_entry(std::string v) {
    const size_t comma = v.find(',');
    if (comma != std::string::npos) v.resize(comma);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    size_t b = 0;
    while (b < v.size() && (v[b] == ' ' || v[b] == '\t')) ++b;
    return v.substr(b);
}

// True when the browser reached us over HTTPS through a reverse proxy:
// the proxy says so in X-Forwarded-Proto (the first entry, when proxies
// are chained). budyk itself speaks plain HTTP, and the start-up warning
// for a non-loopback listen address recommends exactly such a proxy.
// A client that sends the header itself only makes its own cookie
// stricter.
bool via_https(const HttpRequest& req) {
    return ascii_ieq(first_entry(req.header("X-Forwarded-Proto")), "https");
}

// Whether a WebSocket upgrade comes from the dashboard's own page. A
// browser always sends Origin on one, and a page on another site can open
// a WebSocket to 127.0.0.1 as easily as the dashboard can: without this
// it read the live stream (host name, processes, mounts, alerts) and held
// the daemon at L3. The Origin's host[:port] must be the Host the request
// was sent to, or the X-Forwarded-Host of a proxy that rewrote Host. No
// Origin: not a browser (a script, a test), let in as before.
bool same_origin(const HttpRequest& req) {
    const std::string origin = req.header("Origin");
    if (origin.empty()) return true;
    const size_t sep = origin.find("://");
    if (sep == std::string::npos) return false;          // "null", or not a URL
    std::string authority = origin.substr(sep + 3);
    const size_t slash = authority.find('/');
    if (slash != std::string::npos) authority.resize(slash);
    if (authority.empty()) return false;
    for (const char* h : { "Host", "X-Forwarded-Host" }) {
        const std::string v = first_entry(req.header(h));
        if (!v.empty() && ascii_ieq(v, authority.c_str())) return true;
    }
    return false;
}

// The name in a Host header: without the port, the brackets of an IPv6
// address or a trailing dot.
std::string host_name(std::string h) {
    if (!h.empty() && h[0] == '[') {
        const size_t close = h.find(']');
        return close == std::string::npos ? std::string() : h.substr(1, close - 1);
    }
    const size_t colon = h.find(':');
    if (colon != std::string::npos) h.resize(colon);
    if (!h.empty() && h.back() == '.') h.pop_back();
    return h;
}

// Whether a request was sent to one of budyk's own names. Listening on
// loopback without a password, budyk is open to the local user only; but
// a page on another site can point a name of its own at 127.0.0.1 once
// it has loaded (DNS rebinding), and the browser then takes budyk for
// that site: the page reads the API and the WebSocket as its own origin,
// so the Origin check passes too. Such a request carries that name in
// Host. budyk's names are an IP address, localhost and *.localhost
// (which browsers keep to loopback), this host's name and the names in
// web.allowed_hosts (a reverse proxy's). X-Forwarded-Host does not count:
// the page sets it as it likes. No Host (not a browser) is let in. With
// a password on (a page under another name has no session), or on a
// network address (open to the network anyway), any name is.
bool own_host(const HttpRequest& req, const Config& cfg, const std::string& hostname) {
    if (cfg.auth_enabled || std::strncmp(cfg.listen_addr, "127.", 4) != 0) return true;
    const std::string raw = req.header("Host");
    if (raw.empty()) return true;
    const std::string h = host_name(raw);
    if (h.empty()) return false;
    unsigned char addr[16];
    if (::inet_pton(AF_INET, h.c_str(), addr) == 1 || ::inet_pton(AF_INET6, h.c_str(), addr) == 1) {
        return true;
    }
    if (ascii_ieq(h, "localhost")) return true;
    if (h.size() > 10 && ascii_ieq(h.substr(h.size() - 10), ".localhost")) return true;
    if (!hostname.empty() && ascii_ieq(h, hostname.c_str())) return true;
    for (const std::string& allowed : cfg.web_allowed_hosts) {
        if (ascii_ieq(h, allowed.c_str())) return true;
    }
    return false;
}

// Attributes of the session cookie: HttpOnly and SameSite=Strict always,
// Secure when the request came over HTTPS, so the browser never sends
// the bearer token over plain HTTP.
std::string cookie_attrs(const HttpRequest& req) {
    std::string a = "; HttpOnly; Path=/; SameSite=Strict";
    if (via_https(req)) a += "; Secure";
    return a;
}

} // namespace

HttpHandler make_router(const RouterDeps& d) {
    return [d](const HttpRequest& req) -> HttpResponse {
        const Config&   cfg           = *d.cfg;
        HotBuffer&      hot           = *d.hot;
        std::mutex&     hot_mtx       = *d.hot_mtx;
        TierManager&    tm            = *d.tm;
        SessionStore&   sessions      = *d.sessions;
        LoginLimiter&   login_limiter = *d.limiter;
        WebSocketHub&   ws            = *d.ws;
        const Scheduler& sched        = *d.sched;

        if (!own_host(req, cfg, d.hostname)) {
            return budyk::HttpResponse{
                403, "text/plain",
                "budyk: this Host is not one of budyk's names; add it to web.allowed_hosts\n"};
        }

        // touch(): a session in use is kept going (sliding expiration);
        // one left alone for the TTL ends. The dashboard's reloads and
        // the TUI's polls are such use; an open WebSocket alone is not.
        auto authed = [&cfg, &sessions](const HttpRequest& r) -> bool {
            if (!cfg.auth_enabled) return true;
            const std::string c = r.header("Cookie");
            if (c.empty())        return false;
            const std::string tok = cookie_value(c, "budyk_session");
            return !tok.empty() && sessions.touch(tok);
        };

        // Static SPA — served at / and /index.html. Always public; the JS
        // itself does the auth-probe + login round-trip. Its URLs are
        // relative, so a reverse proxy may serve it under a path (/budyk/).
        if (req.method == "GET" &&
            (req.path == "/" || req.path == "/index.html")) {
            budyk::HttpResponse r;
            r.status       = 200;
            r.content_type = "text/html; charset=utf-8";
            r.body.assign(budyk::kSpaIndexHtml, budyk::kSpaIndexHtmlLen);
            return r;
        }

        // Health is always public so liveness probes work pre-auth, so it
        // says only what a probe needs: no paths or other host details.
        if (req.method == "GET" && req.path == "/api/health") {
            budyk::HttpResponse r;
            r.status       = 200;
            r.content_type = "application/json";
            // auth: whether the dashboard has a password, so it can offer
            // to sign out (an API request without a session says it too).
            r.body         = std::string("{\"status\":\"ok\",\"version\":\"") + d.version +
                             "\",\"hostname\":\"" + budyk::json_escape(d.hostname) +
                             "\",\"auth\":" + (cfg.auth_enabled ? "true" : "false") + "}\n";
            return r;
        }

        if (req.method == "POST" && req.path == "/api/auth/login") {
            if (!cfg.auth_enabled || cfg.password_hash[0] == '\0') {
                return budyk::HttpResponse{
                    403, "text/plain", "auth disabled\n"};
            }
            // Throttled before the body is even looked at, so a blocked
            // client costs no Argon2 run.
            const uint64_t now_s = static_cast<uint64_t>(::time(nullptr));
            if (const int wait = login_limiter.retry_after(req.peer, now_s); wait > 0) {
                budyk::HttpResponse r;
                r.status       = 429;
                r.content_type = "application/json";
                r.body         = "{\"error\":\"too many attempts\",\"retry_after\":" +
                                 std::to_string(wait) + "}\n";
                r.extra_headers.push_back({"Retry-After", std::to_string(wait)});
                return r;
            }
            std::string pw;
            if (!budyk::json_get_string(req.body, "password", &pw) || pw.empty()) {
                return budyk::HttpResponse{
                    400, "application/json",
                    "{\"error\":\"missing password\"}\n"};
            }
            if (budyk::argon2_verify(pw, cfg.password_hash) != 0) {
                if (login_limiter.record_failure(req.peer, now_s)) {
                    std::fprintf(stderr,
                        "budyk serve: %d failed logins from %s; refusing attempts for %d s\n",
                        login_limiter.max_failures(), req.peer.c_str(),
                        login_limiter.window_sec());
                }
                return budyk::HttpResponse{
                    401, "application/json",
                    "{\"error\":\"invalid credentials\"}\n"};
            }
            login_limiter.record_success(req.peer);
            const std::string tok = sessions.create();
            if (tok.empty()) {
                return budyk::HttpResponse{
                    500, "application/json",
                    "{\"error\":\"entropy unavailable\"}\n"};
            }
            budyk::HttpResponse r;
            r.status       = 200;
            r.content_type = "application/json";
            r.body         = "{\"ok\":true}\n";
            r.extra_headers.push_back({"Set-Cookie",
                "budyk_session=" + tok + cookie_attrs(req)});
            return r;
        }

        if (req.method == "POST" && req.path == "/api/auth/logout") {
            const std::string c = req.header("Cookie");
            if (!c.empty()) {
                const std::string tok = cookie_value(c, "budyk_session");
                if (!tok.empty()) sessions.revoke(tok);
            }
            budyk::HttpResponse r{200, "application/json", "{\"ok\":true}\n"};
            r.extra_headers.push_back({"Set-Cookie",
                "budyk_session=" + cookie_attrs(req) + "; Max-Age=0"});
            return r;
        }

        if (req.method == "GET" && req.path == "/api/samples") {
            if (!authed(req)) {
                return budyk::HttpResponse{
                    401, "application/json", "{\"error\":\"unauthenticated\"}\n"};
            }
            // A poller (the TUI) counts as a connected client. Wake the loop
            // only when one appears, not on every poll, so an L3 cadence
            // isn't disturbed by extra ticks.
            {
                const uint64_t now  = now_realtime_ns();
                const uint64_t prev = d.last_poll_ns->exchange(now);
                if (prev == 0 || now < prev || now - prev > d.poller_window_ns) {
                    if (d.wake) d.wake();
                }
            }
            // ?last=N: the newest N only. The TUI, drawing one sample a
            // second, took the whole buffer each time (250 KB at the
            // default 300; at the most, 100 000, the HTTP thread's whole
            // second).
            const uint64_t last = query_u64(req.query, "last", 0);
            std::vector<budyk::Sample> snap;
            std::string detail;
            {
                std::lock_guard<std::mutex> g(hot_mtx);
                const size_t have = hot.size();
                snap.resize(last > 0 && last < have ? static_cast<size_t>(last) : have);
                if (!snap.empty()) hot.dump_last(snap.data(), snap.size());
                if (d.latest_detail != nullptr) detail = *d.latest_detail;
            }
            budyk::HttpResponse r;
            r.status       = 200;
            r.content_type = "application/json";
            r.body         = budyk::samples_to_json(snap.data(), snap.size(), &detail);
            return r;
        }

        // Historical range query against the on-disk tier rings — lets
        // the SPA show hours/days, not just the 300-record hot buffer.
        //   GET /api/range?since=<ns>&until=<ns>&tier=<1|2|3>&limit=<n>
        //     since  — lower bound on timestamp_nanos (default 0 = all)
        //     until  — upper bound, 0 = now (default 0)
        //     tier   — 1 raw L3 / 2 1-min L2 / 3 5-min L1 (default 1)
        //     limit  — max samples returned, newest kept (default+cap 5000);
        //              with level=all, thinned over the window, the newest
        //              sample always included
        {
            const std::string& rquery = req.query;
            if (req.method == "GET" && req.path == "/api/range") {
                if (!authed(req)) {
                    return budyk::HttpResponse{
                        401, "application/json",
                        "{\"error\":\"unauthenticated\"}\n"};
                }
                constexpr uint64_t kMaxLimit = 5000;
                const uint64_t since = query_u64(rquery, "since", 0);
                const uint64_t until = query_u64(rquery, "until", 0);
                uint64_t tier        = query_u64(rquery, "tier",  1);
                uint64_t limit       = query_u64(rquery, "limit", kMaxLimit);
                if (tier < 1 || tier > 3) tier = 1;
                if (limit == 0 || limit > kMaxLimit) limit = kMaxLimit;
                // level=<name> (L1..L3 or a custom level) takes precedence
                // over tier and is the only way to read a custom level;
                // level=all merges every ring (see TierManager::query_all).
                const std::string level_name = query_str(rquery, "level");

                std::vector<budyk::Sample> out;
                int n = -1;
                if (level_name == "all") {
                    n = tm.query_all(since, until, static_cast<size_t>(limit), &out);
                } else if (!level_name.empty()) {
                    budyk::Level lv;
                    if (!sched.level_by_name(level_name, &lv)) {
                        return budyk::HttpResponse{
                            404, "application/json",
                            "{\"error\":\"unknown level\"}\n"};
                    }
                    n = tm.query_level(lv, since, until,
                                       static_cast<size_t>(limit), &out);
                } else {
                    n = tm.query(static_cast<int>(tier), since, until,
                                 static_cast<size_t>(limit), &out);
                }
                if (n < 0) {
                    return budyk::HttpResponse{
                        400, "application/json",
                        "{\"error\":\"bad range query\"}\n"};
                }
                budyk::HttpResponse r;
                r.status       = 200;
                r.content_type = "application/json";
                r.body         = budyk::samples_to_json(out.data(), out.size());
                return r;
            }
        }

        // The latest alerts the rules raised (RAM only): what a dashboard
        // shows when it opens; new ones arrive over the WebSocket.
        if (req.method == "GET" && req.path == "/api/alerts") {
            if (!authed(req)) {
                return budyk::HttpResponse{
                    401, "application/json", "{\"error\":\"unauthenticated\"}\n"};
            }
            std::vector<budyk::AlertEvent> snap;
            if (d.recent_alerts != nullptr) {
                std::lock_guard<std::mutex> g(*d.hot_mtx);
                snap.assign(d.recent_alerts->begin(), d.recent_alerts->end());
            }
            budyk::HttpResponse r;
            r.status       = 200;
            r.content_type = "application/json";
            r.body         = budyk::alerts_to_json(snap.data(), snap.size());
            return r;
        }

        // Level table, so clients can name the `level` id carried by every
        // sample: [{"id","name","interval_ms","priority","builtin",
        // "collects","metrics"}, ...]; "collects" is false for L1 and L2 in
        // on-demand mode, "metrics" names what the level reads.
        if (req.method == "GET" && req.path == "/api/levels") {
            if (!authed(req)) {
                return budyk::HttpResponse{
                    401, "application/json", "{\"error\":\"unauthenticated\"}\n"};
            }
            std::string body = "[";
            auto add = [&](budyk::Level lv, bool builtin) {
                if (body.size() > 1) body += ",";
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    "{\"id\":%u,\"name\":\"%s\",\"interval_ms\":%d,"
                    "\"priority\":%d,\"builtin\":%s,\"collects\":%s,\"metrics\":[",
                    static_cast<unsigned>(lv), sched.level_name(lv),
                    sched.interval_ms(lv), sched.priority(lv),
                    builtin ? "true" : "false", sched.collects(lv) ? "true" : "false");
                body += buf;
                const auto names = budyk::metric_set_names(sched.metrics(lv));
                for (size_t i = 0; i < names.size(); ++i) {
                    if (i > 0) body += ",";
                    body += "\"" + names[i] + "\"";
                }
                body += "]}";
            };
            add(budyk::Level::L1, true);
            add(budyk::Level::L2, true);
            add(budyk::Level::L3, true);
            for (const auto& c : sched.custom_levels()) {
                add(static_cast<budyk::Level>(c.id), false);
            }
            body += "]\n";
            budyk::HttpResponse r;
            r.status       = 200;
            r.content_type = "application/json";
            r.body         = body;
            return r;
        }

        if (req.path == "/api/ws") {
            if (!budyk::is_websocket_upgrade(req)) {
                return budyk::HttpResponse{
                    400, "text/plain", "expected websocket upgrade\n"};
            }
            if (!same_origin(req)) {
                return budyk::HttpResponse{
                    403, "text/plain", "cross-origin websocket refused\n"};
            }
            if (!authed(req)) {
                return budyk::HttpResponse{
                    401, "text/plain", "unauthenticated\n"};
            }
            if (ws.size() >= budyk::WebSocketHub::kMaxClients) {
                return budyk::HttpResponse{
                    503, "text/plain", "too many websocket clients\n"};
            }
            // The session it was let in with: the hub drops the client
            // when that session ends (logout, expiry).
            const std::string session =
                cfg.auth_enabled ? cookie_value(req.header("Cookie"), "budyk_session") : std::string();
            const std::string key = req.header("Sec-WebSocket-Key");
            const std::string handshake = budyk::ws_handshake_response(key);

            // Snapshot the hot buffer right now so the new client gets
            // history immediately on connect (catch-up frame).
            std::vector<budyk::Sample> snap;
            {
                std::lock_guard<std::mutex> g(hot_mtx);
                snap.resize(hot.size());
                if (!snap.empty()) hot.dump(snap.data(), snap.size());
            }
            std::string catchup = budyk::ws_text_frame(
                budyk::samples_to_json(snap.data(), snap.size()));

            budyk::HttpResponse r;
            // Sent within the server's bounds, as a response is: a
            // blocking send() to a client that read a byte every few
            // seconds held the HTTP thread for the whole catch-up frame.
            r.hijack = [handshake, catchup, session, &ws, wake = d.wake,
                        io = d.io_timeout_ms, rate = d.min_send_rate](int fd) {
                if (budyk::send_bounded(fd, handshake.data(), handshake.size(), io, rate) < 0 ||
                    budyk::send_bounded(fd, catchup.data(), catchup.size(), io, rate) < 0) {
                    ::close(fd);
                    return;
                }
                ws.add(fd, session);
                if (wake) wake();         // step up to L3 now, not after the sleep
            };
            return r;
        }
        return budyk::HttpResponse{404, "text/plain", "not found\n"};
    };
}

} // namespace budyk
