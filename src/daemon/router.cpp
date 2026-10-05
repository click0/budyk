// SPDX-License-Identifier: BSD-3-Clause
#include "daemon/router.h"

#include "core/clock.h"
#include "web/auth.h"
#include "web/http_util.h"
#include "web/json.h"
#include "web/spa.h"

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace budyk {

namespace {

// True when the browser reached us over HTTPS through a reverse proxy:
// the proxy says so in X-Forwarded-Proto (the first entry, when proxies
// are chained). budyk itself speaks plain HTTP, and the start-up warning
// for a non-loopback listen address recommends exactly such a proxy.
// A client that sends the header itself only makes its own cookie
// stricter.
bool via_https(const HttpRequest& req) {
    std::string proto = req.header("X-Forwarded-Proto");
    const size_t comma = proto.find(',');
    if (comma != std::string::npos) proto.resize(comma);
    while (!proto.empty() && (proto.back() == ' ' || proto.back() == '\t')) proto.pop_back();
    size_t b = 0;
    while (b < proto.size() && (proto[b] == ' ' || proto[b] == '\t')) ++b;
    return ascii_ieq(proto.substr(b), "https");
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

        auto authed = [&cfg, &sessions](const HttpRequest& r) -> bool {
            if (!cfg.auth_enabled) return true;
            const std::string c = r.header("Cookie");
            if (c.empty())        return false;
            const std::string tok = cookie_value(c, "budyk_session");
            return !tok.empty() && sessions.verify(tok);
        };

        // Static SPA — served at /, /index.html and /budyk for the
        // browser-friendly entry. Always public; the JS itself does
        // the auth-probe + login round-trip.
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
            r.body         = std::string("{\"status\":\"ok\",\"version\":\"") + d.version + "\"}\n";
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
            std::vector<budyk::Sample> snap;
            {
                std::lock_guard<std::mutex> g(hot_mtx);
                snap.resize(hot.size());
                if (!snap.empty()) hot.dump(snap.data(), snap.size());
            }
            budyk::HttpResponse r;
            r.status       = 200;
            r.content_type = "application/json";
            r.body         = budyk::samples_to_json(snap.data(), snap.size());
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
            std::string rpath, rquery;
            split_target(req.path, &rpath, &rquery);
            // cppcheck can't see split_target fill rpath through the pointer.
            // cppcheck-suppress knownConditionTrueFalse
            if (req.method == "GET" && rpath == "/api/range") {
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

        // Level table, so clients can name the `level` id carried by every
        // sample: [{"id","name","interval_ms","priority","builtin"}, ...].
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
                    "\"priority\":%d,\"builtin\":%s}",
                    static_cast<unsigned>(lv), sched.level_name(lv),
                    sched.interval_ms(lv), sched.priority(lv),
                    builtin ? "true" : "false");
                body += buf;
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
            if (!authed(req)) {
                return budyk::HttpResponse{
                    401, "text/plain", "unauthenticated\n"};
            }
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
            r.hijack = [handshake, catchup, &ws, wake = d.wake](int fd) {
                ssize_t n1 = ::send(fd, handshake.data(), handshake.size(), MSG_NOSIGNAL);
                if (n1 != static_cast<ssize_t>(handshake.size())) {
                    ::close(fd);
                    return;
                }
                ssize_t n2 = ::send(fd, catchup.data(), catchup.size(), MSG_NOSIGNAL);
                if (n2 != static_cast<ssize_t>(catchup.size())) {
                    ::close(fd);
                    return;
                }
                ws.add(fd);
                if (wake) wake();         // step up to L3 now, not after the sleep
            };
            return r;
        }
        return budyk::HttpResponse{404, "text/plain", "not found\n"};
    };
}

} // namespace budyk
