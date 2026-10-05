// SPDX-License-Identifier: BSD-3-Clause
//
// The daemon's HTTP routes, called directly through the handler that
// make_router() returns — no socket, no daemon. Real collaborators
// (hot buffer, tier manager on a temp dir, session store, limiter,
// hub, scheduler), so what is checked is what the daemon serves.
#include "daemon/router.h"

#include "core/sample.h"
#include "web/auth.h"
#include "web/http_util.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>

using namespace budyk;

namespace {

struct World {
    Config                 cfg;
    HotBuffer              hot{16};
    std::mutex             hot_mtx;
    TierManager            tm;
    SessionStore           sessions;
    LoginLimiter           limiter{2, 60};     // two failures, then 429
    WebSocketHub           ws;
    Scheduler              sched;
    std::atomic<uint64_t>  last_poll{0};
    int                    wakes = 0;
    std::string            dir;
    HttpHandler            handler;

    World() : sched(cfg.scheduler) {}

    void init() {
        char tmpl[] = "/tmp/budyk_router_XXXXXX";
        dir = ::mkdtemp(tmpl);
        assert(!dir.empty());
        assert(tm.init(dir.c_str(), 1, 1, 1) == 0);
        RouterDeps d;
        d.cfg = &cfg; d.hot = &hot; d.hot_mtx = &hot_mtx; d.tm = &tm;
        d.sessions = &sessions; d.limiter = &limiter; d.ws = &ws; d.sched = &sched;
        d.last_poll_ns = &last_poll;
        d.poller_window_ns = 5ULL * 1000000000ULL;
        d.wake = [this] { ++wakes; };
        d.version = "9.9.9-test";
        handler = make_router(d);
    }
    ~World() {
        tm.close();
        for (const char* f : {"tier1.ring", "tier2.ring", "tier3.ring"}) {
            ::unlink((dir + "/" + f).c_str());
        }
        ::rmdir(dir.c_str());
    }
};

HttpRequest get(const char* path, const std::string& cookie = "") {
    HttpRequest r;
    r.method = "GET";
    r.path   = path;
    r.peer   = "127.0.0.1";
    if (!cookie.empty()) r.headers.push_back({"Cookie", cookie});
    return r;
}

HttpRequest post(const char* path, const std::string& body, const std::string& peer = "127.0.0.1") {
    HttpRequest r;
    r.method = "POST";
    r.path   = path;
    r.body   = body;
    r.peer   = peer;
    return r;
}

std::string header(const HttpResponse& r, const char* name) {
    for (const auto& kv : r.extra_headers) if (kv.first == name) return kv.second;
    return {};
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

} // namespace

int main() {
    // 1. Public routes: the SPA, /api/health with the version, 404 for
    //    the rest, and a method that doesn't match.
    {
        World w; w.init();
        HttpResponse r = w.handler(get("/"));
        assert(r.status == 200 && has(r.content_type, "text/html") && has(r.body, "<html"));
        assert(w.handler(get("/index.html")).status == 200);
        r = w.handler(get("/api/health"));
        assert(r.status == 200 && r.body == "{\"status\":\"ok\",\"version\":\"9.9.9-test\"}\n");
        assert(!has(r.body, "data_dir"));
        assert(w.handler(get("/nope")).status == 404);
        assert(w.handler(post("/api/health", "")).status == 404);
    }

    // 2. Auth disabled: login is refused (403), everything else is open.
    {
        World w; w.init();
        assert(w.handler(post("/api/auth/login", "{\"password\":\"x\"}")).status == 403);
        assert(w.handler(get("/api/samples")).status == 200);
        assert(w.handler(get("/api/levels")).status == 200);
        assert(w.handler(get("/api/range")).status == 200);
    }

    // 3. Auth enabled: the login flow, the throttle, the cookie, logout.
    {
        World w;
        w.cfg.auth_enabled = true;
        std::string hash;
        assert(argon2_hash("hunter2", Argon2Params{}, &hash) == 0);
        std::snprintf(w.cfg.password_hash, sizeof(w.cfg.password_hash), "%s", hash.c_str());
        w.init();

        assert(w.handler(get("/api/samples")).status == 401);
        assert(w.handler(get("/api/levels")).status  == 401);
        assert(w.handler(get("/api/range")).status   == 401);

        assert(w.handler(post("/api/auth/login", "{}")).status == 400);
        assert(w.handler(post("/api/auth/login", "{\"password\":\"\"}")).status == 400);
        assert(w.handler(post("/api/auth/login", "{\"password\":\"wrong\"}")).status == 401);
        assert(w.handler(post("/api/auth/login", "{\"password\":\"wrong\"}")).status == 401);
        // Third attempt from this address: throttled before the hash.
        HttpResponse r = w.handler(post("/api/auth/login", "{\"password\":\"hunter2\"}"));
        assert(r.status == 429);
        assert(!header(r, "Retry-After").empty());
        assert(has(r.body, "retry_after"));
        // Another address is not.
        r = w.handler(post("/api/auth/login", "{\"password\":\"hunter2\"}", "10.0.0.2"));
        assert(r.status == 200);
        const std::string set = header(r, "Set-Cookie");
        assert(has(set, "budyk_session=") && has(set, "HttpOnly") && has(set, "SameSite=Strict"));
        const std::string tok = cookie_value(set, "budyk_session");
        assert(!tok.empty());
        const std::string cookie = "budyk_session=" + tok;

        assert(w.handler(get("/api/samples", cookie)).status == 200);
        assert(w.handler(get("/api/levels", cookie)).status  == 200);
        assert(w.handler(get("/api/samples", "budyk_session=forged")).status == 401);

        // Plain HTTP: no Secure (the browser would drop the cookie).
        assert(!has(set, "Secure"));
        // Behind a TLS proxy: Secure, whatever the case or a proxy chain.
        for (const char* proto : {"https", "HTTPS", "https, http", " https "}) {
            HttpRequest via = post("/api/auth/login", "{\"password\":\"hunter2\"}", "10.0.0.3");
            via.headers.push_back({"X-Forwarded-Proto", proto});
            r = w.handler(via);
            assert(r.status == 200);
            assert(has(header(r, "Set-Cookie"), "; Secure"));
            assert(has(header(r, "Set-Cookie"), "HttpOnly"));
        }
        for (const char* proto : {"http", "http, https", ""}) {
            HttpRequest via = post("/api/auth/login", "{\"password\":\"hunter2\"}", "10.0.0.3");
            via.headers.push_back({"X-Forwarded-Proto", proto});
            r = w.handler(via);
            assert(r.status == 200);
            assert(!has(header(r, "Set-Cookie"), "Secure"));
        }

        HttpRequest lo = post("/api/auth/logout", "");
        lo.headers.push_back({"Cookie", cookie});
        lo.headers.push_back({"X-Forwarded-Proto", "https"});
        r = w.handler(lo);
        assert(r.status == 200 && has(header(r, "Set-Cookie"), "Max-Age=0"));
        assert(has(header(r, "Set-Cookie"), "; Secure"));
        assert(has(header(r, "Set-Cookie"), "budyk_session=;"));
        assert(w.handler(get("/api/samples", cookie)).status == 401);   // revoked
        // Logout without a cookie is harmless.
        assert(w.handler(post("/api/auth/logout", "")).status == 200);
    }

    // 4. /api/samples serves the hot buffer, records the poll and wakes
    //    the loop once per quiet spell, not on every poll.
    {
        World w; w.init();
        Sample s{};
        s.timestamp_nanos = 123456789;
        s.level = Level::L3;
        s.cpu.total_percent = 42.5;
        { std::lock_guard<std::mutex> g(w.hot_mtx); w.hot.push(s); }
        HttpResponse r = w.handler(get("/api/samples"));
        assert(r.status == 200 && has(r.body, "\"ts\":123456789") && has(r.body, "42.5"));
        assert(w.last_poll.load() != 0);
        assert(w.wakes == 1);
        w.handler(get("/api/samples"));
        assert(w.wakes == 1);                         // within the window: no second wake
        w.last_poll.store(1);                         // long ago
        w.handler(get("/api/samples"));
        assert(w.wakes == 2);
    }

    // 5. /api/levels lists the built-in levels in order with their ids.
    {
        World w; w.init();
        HttpResponse r = w.handler(get("/api/levels"));
        assert(r.status == 200);
        assert(has(r.body, "{\"id\":1,\"name\":\"L1\""));
        assert(has(r.body, "{\"id\":2,\"name\":\"L2\""));
        assert(has(r.body, "{\"id\":3,\"name\":\"L3\""));
        assert(has(r.body, "\"builtin\":true"));
        assert(r.body.find("\"builtin\":false") == std::string::npos);
    }

    // 6. /api/range: what is stored comes back; parameters are clamped
    //    and bad ones fall back; an unknown level is 404; level wins
    //    over tier.
    {
        World w; w.init();
        for (uint64_t ts = 1000; ts <= 1005; ++ts) {
            Sample s{};
            s.timestamp_nanos = ts;
            s.level = Level::L3;
            assert(w.tm.store(s) == 0);
        }
        Sample l1{};
        l1.timestamp_nanos = 2000;
        l1.level = Level::L1;
        assert(w.tm.store(l1) == 0);

        HttpResponse r = w.handler(get("/api/range"));
        assert(r.status == 200 && has(r.body, "\"count\":6"));            // tier 1 = L3 ring
        r = w.handler(get("/api/range?tier=3"));
        assert(r.status == 200 && has(r.body, "\"count\":1") && has(r.body, "\"ts\":2000"));
        r = w.handler(get("/api/range?tier=9"));
        assert(r.status == 200 && has(r.body, "\"count\":6"));            // clamped to 1
        r = w.handler(get("/api/range?since=1003&until=1004"));
        assert(r.status == 200 && has(r.body, "\"count\":2"));
        r = w.handler(get("/api/range?limit=2"));
        assert(r.status == 200 && has(r.body, "\"count\":2") && has(r.body, "\"ts\":1005"));
        r = w.handler(get("/api/range?limit=abc&since=-5"));
        assert(r.status == 200 && has(r.body, "\"count\":6"));            // fallbacks
        r = w.handler(get("/api/range?level=L1&tier=1"));
        assert(r.status == 200 && has(r.body, "\"count\":1") && has(r.body, "\"ts\":2000"));
        r = w.handler(get("/api/range?level=all"));
        assert(r.status == 200 && has(r.body, "\"count\":7"));
        r = w.handler(get("/api/range?level=nope"));
        assert(r.status == 404 && has(r.body, "unknown level"));
    }

    // 7. /api/ws: not an upgrade → 400; an upgrade when unauthenticated
    //    → 401; a valid upgrade hands the connection over (hijack set,
    //    nothing written as a normal response).
    {
        World w; w.init();
        assert(w.handler(get("/api/ws")).status == 400);
        HttpRequest up = get("/api/ws");
        up.headers.push_back({"Upgrade", "websocket"});
        up.headers.push_back({"Connection", "Upgrade"});
        up.headers.push_back({"Sec-WebSocket-Version", "13"});
        up.headers.push_back({"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="});
        HttpResponse r = w.handler(up);
        assert(static_cast<bool>(r.hijack));
        assert(r.body.empty());

        w.cfg.auth_enabled = true;
        std::snprintf(w.cfg.password_hash, sizeof(w.cfg.password_hash), "%s", "$argon2id$v=19$dummy");
        r = w.handler(up);
        assert(r.status == 401 && !r.hijack);
    }

    std::printf("test_router: PASS\n");
    return 0;
}
