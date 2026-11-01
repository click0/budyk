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
#include <deque>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

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
    std::deque<AlertEvent> alerts;
    int                    wakes = 0;
    std::string            dir;
    HttpHandler            handler;
    int                    io_timeout_ms = 5000;        // the server's bounds, for the WebSocket upgrade
    size_t                 min_send_rate = 256 * 1024;

    World() : sched(cfg.scheduler) {}
    explicit World(const SchedulerConfig& sc) : sched(sc) {}
    World(size_t hot_cap, uint64_t session_ttl_s)
        : hot(hot_cap), sessions(session_ttl_s), sched(cfg.scheduler) {}

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
        d.hostname = "db\"1";                // escaped in the JSON
        d.recent_alerts = &alerts;
        d.io_timeout_ms = io_timeout_ms;
        d.min_send_rate = min_send_rate;
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

// Requests as the HTTP server hands them over: the target already split
// into path and query.
HttpRequest get(const char* target, const std::string& cookie = "") {
    HttpRequest r;
    r.method = "GET";
    split_target(target, &r.path, &r.query);
    r.peer   = "127.0.0.1";
    if (!cookie.empty()) r.headers.push_back({"Cookie", cookie});
    return r;
}

HttpRequest post(const char* target, const std::string& body, const std::string& peer = "127.0.0.1") {
    HttpRequest r;
    r.method = "POST";
    split_target(target, &r.path, &r.query);
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
    // 1. Public routes: the SPA, /api/health with the version and the
    //    hostname, 404 for the rest, and a method that doesn't match.
    {
        World w; w.init();
        HttpResponse r = w.handler(get("/"));
        assert(r.status == 200 && has(r.content_type, "text/html") && has(r.body, "<html"));
        // Its URLs are relative: behind a proxy under a path, "/api/..."
        // went to the proxy's own /api.
        assert(!has(r.body, "fetch(\"/api") && !has(r.body, "`/api") && !has(r.body, "host}/api"));
        assert(w.handler(get("/index.html")).status == 200);
        r = w.handler(get("/api/health"));
        assert(r.status == 200 &&
               r.body == "{\"status\":\"ok\",\"version\":\"9.9.9-test\",\"hostname\":\"db\\\"1\","
                         "\"auth\":false}\n");
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
        assert(w.handler(get("/api/alerts")).status  == 401);
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

        // ?last=N: the newest N (the TUI asks for 1, it drew one and was
        // sent the whole buffer each second); more than there are is all.
        for (const uint64_t ts : {uint64_t{200}, uint64_t{300}}) {
            s.timestamp_nanos = ts;
            std::lock_guard<std::mutex> g(w.hot_mtx);
            w.hot.push(s);
        }
        r = w.handler(get("/api/samples?last=1"));
        assert(r.status == 200 && has(r.body, "\"ts\":300") && !has(r.body, "\"ts\":200") &&
               !has(r.body, "\"ts\":123456789"));
        r = w.handler(get("/api/samples?last=2"));
        assert(has(r.body, "\"ts\":200") && has(r.body, "\"ts\":300") &&
               !has(r.body, "\"ts\":123456789"));
        r = w.handler(get("/api/samples?last=99"));
        assert(has(r.body, "\"ts\":123456789") && has(r.body, "\"ts\":300"));
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

    // 7b. /api/ws from another site's page (CSWSH): a browser always sends
    //     Origin on a WebSocket, and one that is not the Host the request
    //     went to (or a proxy's X-Forwarded-Host) is refused with 403.
    //     Past WebSocketHub::kMaxClients, 503. Both used to get the stream.
    {
        World w; w.init();
        auto upgrade = [](const char* origin, const char* host) {
            HttpRequest up = get("/api/ws");
            up.headers.push_back({"Upgrade", "websocket"});
            up.headers.push_back({"Connection", "Upgrade"});
            up.headers.push_back({"Sec-WebSocket-Version", "13"});
            up.headers.push_back({"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="});
            if (host != nullptr)   up.headers.push_back({"Host", host});
            if (origin != nullptr) up.headers.push_back({"Origin", origin});
            return up;
        };
        HttpResponse r = w.handler(upgrade("http://evil.example", "127.0.0.1:8080"));
        assert(r.status == 403 && !r.hijack);
        assert(w.handler(upgrade("null", "127.0.0.1:8080")).status == 403);
        assert(w.handler(upgrade("http://127.0.0.1:8081", "127.0.0.1:8080")).status == 403);
        assert(static_cast<bool>(w.handler(upgrade("http://127.0.0.1:8080", "127.0.0.1:8080")).hijack));
        assert(static_cast<bool>(w.handler(upgrade("http://LocalHost:8080", "localhost:8080")).hijack));
        assert(static_cast<bool>(w.handler(upgrade(nullptr, "127.0.0.1:8080")).hijack));   // not a browser
        HttpRequest proxied = upgrade("https://budyk.example.com", "127.0.0.1:8080");
        proxied.headers.push_back({"X-Forwarded-Host", "budyk.example.com"});
        assert(static_cast<bool>(w.handler(proxied).hijack));

        std::vector<int> peers;
        for (size_t i = 0; i < WebSocketHub::kMaxClients; ++i) {
            int sp[2];
            assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
            w.ws.add(sp[0]);
            peers.push_back(sp[1]);
        }
        r = w.handler(upgrade(nullptr, "127.0.0.1:8080"));
        assert(r.status == 503 && !r.hijack);
        w.ws.close_all();
        for (const int p : peers) ::close(p);
    }

    // 7c. DNS rebinding: on loopback without a password, a page whose own
    //     name now points at 127.0.0.1 reads the API (and the WebSocket,
    //     Origin and Host agreeing) as its own origin. Its name is in
    //     Host: only an address, localhost, this host's name or
    //     web.allowed_hosts are let in. X-Forwarded-Host, which the page
    //     sets itself, does not count. With a password on, or on a
    //     network address, any name is.
    {
        World w;
        w.cfg.web_allowed_hosts = {"budyk.example.com"};
        w.init();
        auto at = [](const char* path, const char* host) {
            HttpRequest r = get(path);
            if (host != nullptr) r.headers.push_back({"Host", host});
            return r;
        };
        for (const char* host : {"127.0.0.1:8080", "127.0.0.1", "[::1]:8080", "localhost:8080",
                                 "LOCALHOST", "localhost.", "dash.localhost:8080",
                                 "db\"1", "budyk.example.com", "Budyk.Example.Com:443"}) {
            assert(w.handler(at("/api/samples", host)).status == 200);
        }
        assert(w.handler(at("/api/samples", nullptr)).status == 200);       // not a browser
        for (const char* host : {"evil.example:8080", "evil.example", "localhost.evil.example",
                                 "127.0.0.1.evil.example", "[::1", "budyk.example.com.evil",
                                 ":8080"}) {
            HttpResponse r = w.handler(at("/api/samples", host));
            assert(r.status == 403 && has(r.body, "web.allowed_hosts"));
        }
        HttpRequest forged = at("/api/samples", "evil.example:8080");
        forged.headers.push_back({"X-Forwarded-Host", "127.0.0.1:8080"});
        assert(w.handler(forged).status == 403);
        HttpRequest ws = at("/api/ws", "evil.example:8080");
        ws.headers.push_back({"Upgrade", "websocket"});
        ws.headers.push_back({"Connection", "Upgrade"});
        ws.headers.push_back({"Sec-WebSocket-Version", "13"});
        ws.headers.push_back({"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="});
        ws.headers.push_back({"Origin", "http://evil.example:8080"});      // same origin
        HttpResponse r = w.handler(ws);
        assert(r.status == 403 && !r.hijack);
        assert(w.handler(at("/", "evil.example:8080")).status == 403);

        World open; std::snprintf(open.cfg.listen_addr, sizeof(open.cfg.listen_addr), "0.0.0.0");
        open.init();
        assert(open.handler(at("/api/samples", "evil.example:8080")).status == 200);
        World authed; authed.cfg.auth_enabled = true;
        authed.init();
        assert(authed.handler(at("/api/samples", "evil.example:8080")).status == 401);
    }

    // 8. /api/levels: each level's metric set and whether it collects
    //    (L1 and L2 do not in on-demand mode).
    {
        SchedulerConfig sc;
        sc.mode = CollectionMode::OnDemand;
        sc.l1_metrics = metric_set_minimal();
        CustomLevel lv;
        lv.id = kFirstCustomLevel; lv.name = "burst"; lv.interval_ms = 300; lv.priority = 40;
        lv.metrics = MetricSet{kGroupsMinimal | kGroupDisk, kDetailDisks};
        sc.custom_levels.push_back(lv);
        World w(sc);
        w.init();
        const HttpResponse r = w.handler(get("/api/levels"));
        assert(r.status == 200);
        assert(has(r.body, "{\"id\":1,\"name\":\"L1\",\"interval_ms\":300000,\"priority\":0,"
                           "\"builtin\":true,\"collects\":false,"
                           "\"metrics\":[\"cpu\",\"mem\",\"swap\",\"load\",\"uptime\"]}"));
        assert(has(r.body, "\"name\":\"L2\",\"interval_ms\":30000,\"priority\":20,"
                           "\"builtin\":true,\"collects\":false,\"metrics\":[\"cpu\","));
        assert(has(r.body, "\"name\":\"L3\",\"interval_ms\":1000,\"priority\":30,"
                           "\"builtin\":true,\"collects\":true,\"metrics\":[\"cpu\","));
        assert(has(r.body, "\"tcp\",\"udp\",\"fs\",\"clock\",\"icmp\",\"cpu.cores\",\"disk.devices\","
                           "\"net.interfaces\",\"fs.mounts\",\"proc.top\"]}"));
        assert(has(r.body, "{\"id\":4,\"name\":\"burst\",\"interval_ms\":300,\"priority\":40,"
                           "\"builtin\":false,\"collects\":true,"
                           "\"metrics\":[\"cpu\",\"mem\",\"swap\",\"load\",\"disk\",\"uptime\","
                           "\"disk.devices\"]}]\n"));
    }

    // 9. /api/alerts: what the serve loop keeps, oldest first; none yet
    //    is an empty list.
    {
        World w;
        w.init();
        HttpResponse r = w.handler(get("/api/alerts"));
        assert(r.status == 200 && r.body == "{\"count\":0,\"alerts\":[]}\n");
        w.alerts.push_back(AlertEvent{1, "warning", "a", "first"});
        w.alerts.push_back(AlertEvent{2, "critical", "b", "second"});
        r = w.handler(get("/api/alerts"));
        assert(r.status == 200 && r.content_type == "application/json");
        assert(has(r.body, "{\"count\":2,\"alerts\":[{\"ts\":1,\"severity\":\"warning\","
                           "\"rule\":\"a\",\"message\":\"first\"},{\"ts\":2,"));
    }

    // 10. Sliding expiration (spec §3.2): a request with the session cookie
    //     keeps the session going, so one in use does not end at the TTL.
    //     A session ended TTL after the login however much it was used:
    //     the dashboard, or the TUI polling each second, got the login
    //     screen in the middle of the day.
    {
        World w(16, /*session_ttl_s*/ 2);
        w.cfg.auth_enabled = true;
        std::snprintf(w.cfg.password_hash, sizeof(w.cfg.password_hash), "%s", "$argon2id$v=19$dummy");
        w.init();
        const std::string cookie = "budyk_session=" + w.sessions.create();
        assert(w.handler(get("/api/samples", cookie)).status == 200);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));   // past an eighth of the TTL
        assert(w.handler(get("/api/samples", cookie)).status == 200);  // slid: ends at 2.6 s now
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));  // 2.2 s: past the login's TTL
        assert(w.handler(get("/api/samples", cookie)).status == 200);
        assert(w.handler(get("/api/samples", "budyk_session=nope")).status == 401);
    }

    // 11. The WebSocket handshake and the catch-up frame go out within the
    //     server's bounds (io_timeout_ms plus the size at min_send_rate),
    //     as a response does. A client that does not read got a blocking
    //     send(): the HTTP thread sat in it for as long as the client
    //     cared to read a byte at a time. It is dropped now, its socket
    //     closed, and the hub never sees it; one that reads gets both and
    //     joins the hub.
    {
        World w(2000, 86400);
        w.io_timeout_ms = 200;
        w.min_send_rate = 4 * 1024 * 1024;
        w.init();
        Sample s{};
        for (int i = 0; i < 2000; ++i) {          // a catch-up frame of over 1 MB
            s.timestamp_nanos = 1000 + static_cast<uint64_t>(i);
            w.hot.push(s);
        }
        auto upgrade = [] {
            HttpRequest up = get("/api/ws");
            up.headers.push_back({"Upgrade", "websocket"});
            up.headers.push_back({"Connection", "Upgrade"});
            up.headers.push_back({"Sec-WebSocket-Version", "13"});
            up.headers.push_back({"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="});
            return up;
        };
        HttpResponse r = w.handler(upgrade());
        assert(static_cast<bool>(r.hijack));
        int sp[2];
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
        const auto t0 = std::chrono::steady_clock::now();
        r.hijack(sp[0]);                           // the peer reads nothing
        const auto took = std::chrono::steady_clock::now() - t0;
        assert(took < std::chrono::seconds(5));
        assert(w.ws.size() == 0 && w.wakes == 0);
        assert(::fcntl(sp[0], F_GETFD) == -1 && errno == EBADF);   // closed by the hijack
        char head[64];
        const ssize_t n = ::read(sp[1], head, sizeof(head));
        assert(n >= 12 && std::memcmp(head, "HTTP/1.1 101", 12) == 0);
        ::close(sp[1]);

        r = w.handler(upgrade());
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
        std::atomic<size_t> got{0};
        std::thread reader([&] {
            char b[4096];
            ssize_t k;
            while ((k = ::read(sp[1], b, sizeof(b))) > 0) got += static_cast<size_t>(k);
        });
        r.hijack(sp[0]);                           // the peer reads it all
        assert(w.ws.size() == 1 && w.wakes == 1);
        w.ws.close_all();                          // the reader sees the end
        reader.join();
        ::close(sp[1]);
        assert(got.load() > 1000000);              // the handshake and the whole catch-up
    }

    std::printf("test_router: PASS\n");
    return 0;
}
