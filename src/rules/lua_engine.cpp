// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_engine.h"

#include "core/clock.h"

#include "rules/lua_bindings.h"
#include "rules/lua_pattern.h"
#include "rules/lua_stdlib.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cerrno>
#include <climits>
#include <strings.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace budyk {

namespace {

constexpr const char* kEngineRegKey = "budyk.engine";

// Logs a rule's runtime error the first time it appears, so a `when`
// that raises on every tick doesn't flood the log but a new error (or the
// same one after the rule recovered) still shows up. `last` is per rule.
void log_rule_error(std::string* last, const char* rule, const char* part,
                    const char* msg) {
    const char* m = msg != nullptr ? msg : "unknown error";
    if (*last == m) return;
    *last = m;
    std::fprintf(stderr, "budyk: rule '%s' %s failed: %s\n", rule, part, m);
}

// A when() or level condition that read what this level does not
// collect: logged once per distinct message, as a skip, not a failure.
void log_skip(std::string* last, const char* what, const char* msg) {
    const char* m = msg != nullptr ? msg : "unknown error";
    if (*last == m) return;
    *last = m;
    std::fprintf(stderr, "budyk: %s skipped while the level does not collect what it reads: %s\n",
                 what, m);
}

// The error object at `idx` as text, without converting it: a number's
// lua_tostring() allocates, and this runs outside any protected call,
// after a call that failed (perhaps for want of memory).
const char* error_text(lua_State* L, int idx) {
    static char buf[64];
    switch (lua_type(L, idx)) {
        case LUA_TSTRING:
            return lua_tostring(L, idx);
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(lua_tointeger(L, idx)));
            } else {
                std::snprintf(buf, sizeof(buf), "%.14g", static_cast<double>(lua_tonumber(L, idx)));
            }
            return buf;
        default:
            std::snprintf(buf, sizeof(buf), "error object is a %s value", luaL_typename(L, idx));
            return buf;
    }
}

// An error outside any protected call: Lua aborts once this returns.
// Every entry into Lua is protected; this is the message if one is not.
int on_panic(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    std::fprintf(stderr, "budyk: unprotected Lua error, aborting: %s\n",
                 msg != nullptr ? msg : "unknown error");
    return 0;
}

// Calls the library function in upvalue 1 with this call's arguments.
int call_upvalue(lua_State* L) {
    const int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

// string.rep(s, n [, sep]). With s and sep empty, Lua's loops n times
// copying nothing, in C, where no instruction is counted:
// rep("", math.maxinteger) never returned. Otherwise it copies into a
// buffer the memory limit has already allowed.
int limited_rep(lua_State* L) {
    size_t l = 0, lsep = 0;
    luaL_checklstring(L, 1, &l);
    const lua_Integer n = luaL_checkinteger(L, 2);
    luaL_optlstring(L, 3, "", &lsep);
    if (n <= 0 || l + lsep == 0) {
        lua_pushliteral(L, "");
        return 1;
    }
    return call_upvalue(L);
}

// table.move(a1, f, e, t [, a2]): its e - f + 1 moves are charged before
// they run; nil moved onto nil costs Lua nothing, so nothing else
// bounds them.
int limited_move(lua_State* L) {
    const lua_Integer f = luaL_checkinteger(L, 2);
    const lua_Integer e = luaL_checkinteger(L, 3);
    if (e >= f) {
        const uint64_t n = static_cast<uint64_t>(e) - static_cast<uint64_t>(f);
        budyk_lua_charge(L, n == UINT64_MAX ? n : n + 1);
    }
    return call_upvalue(L);
}

// table.concat, insert, remove and sort loop in C over #t or t[i]. A
// __len (any length it likes) or a C function as __index or __newindex
// keeps them there with no instruction counted, so a table with one of
// those is refused. Upvalue 2: the function's name.
int plain_table_only(lua_State* L) {
    if (lua_type(L, 1) == LUA_TTABLE && lua_getmetatable(L, 1) != 0) {
        for (const char* k : { "__len", "__index", "__newindex" }) {
            lua_pushstring(L, k);
            const bool has = lua_rawget(L, -2) != LUA_TNIL;
            lua_pop(L, 1);
            if (has) {
                return luaL_error(L, "table.%s: a table with %s is not supported in rules "
                                     "(the instruction limit cannot see into it)",
                                  lua_tostring(L, lua_upvalueindex(2)), k);
            }
        }
        lua_pop(L, 1);
    }
    // The elements they visit, charged before they run: the loop is in C,
    // one instruction for the call, and 150 000 inserts at the front of a
    // list as long took over a minute. insert(t, pos, v) and remove(t, pos)
    // shift what is after pos, sort compares about n log2 n times, concat
    // reads n.
    if (lua_type(L, 1) == LUA_TTABLE) {
        const auto n = static_cast<uint64_t>(lua_rawlen(L, 1));
        const char* name = lua_tostring(L, lua_upvalueindex(2));
        const bool has_pos = lua_isinteger(L, 2) != 0;
        const lua_Integer pos = has_pos ? lua_tointeger(L, 2) : 0;
        uint64_t work = 0;
        if (std::strcmp(name, "insert") == 0) {
            if (lua_gettop(L) >= 3 && has_pos && pos >= 1 && static_cast<uint64_t>(pos) <= n) {
                work = n - static_cast<uint64_t>(pos) + 1;
            }
        } else if (std::strcmp(name, "remove") == 0) {
            if (has_pos && pos >= 1 && static_cast<uint64_t>(pos) < n) {
                work = n - static_cast<uint64_t>(pos);
            }
        } else if (std::strcmp(name, "sort") == 0) {
            uint64_t bits = 0;
            for (uint64_t m = n; m != 0; m >>= 1) ++bits;
            work = n * bits;
        } else {
            work = n;                                       // concat
        }
        budyk_lua_charge(L, work);
    }
    return call_upvalue(L);
}

// xpcall(f, msgh, ...). Lua calls msgh where the error was raised, and
// for an error the count hook raised that is with hooks still off: a
// msgh that looped ran with no limit at all. Here f runs under a plain
// pcall and msgh after it, as a protected call of its own; past the
// limit msgh does not run, the limit's error goes on up. (No debug
// library in the sandbox, so msgh never needed the erring frames.)
int sandbox_xpcall(lua_State* L) {
    const int n = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    luaL_checkstack(L, n, "too many arguments");
    lua_pushvalue(L, 1);                                  // f
    for (int i = 3; i <= n; ++i) lua_pushvalue(L, i);     // its arguments
    if (lua_pcall(L, n - 2 < 0 ? 0 : n - 2, LUA_MULTRET, 0) == LUA_OK) {
        lua_pushboolean(L, 1);
        lua_insert(L, n + 1);
        return lua_gettop(L) - n;
    }
    budyk_lua_charge(L, 0);                               // raises past the limit
    lua_pushvalue(L, 2);
    lua_insert(L, -2);                                    // msgh, error
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) budyk_lua_charge(L, 0);
    lua_pushboolean(L, 0);
    lua_insert(L, -2);
    return 2;
}

// setmetatable(t, mt) without __gc: Lua runs finalizers with hooks off,
// so a __gc that looped held the tick with no limit (and lua_close() at
// a reload runs every one left). Upvalue 1: the base library's.
int sandbox_setmetatable(lua_State* L) {
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        const bool gc = lua_rawget(L, 2) != LUA_TNIL;
        lua_pop(L, 1);
        if (gc) return luaL_error(L, "setmetatable: __gc is not supported in rules");
    }
    return call_upvalue(L);
}

void open_sandbox_libs(lua_State* L) {
    // Only the safe subset: base + math + string + table.
    luaL_requiref(L, "_G",     luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, "math",   luaopen_math,   1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "table",  luaopen_table,  1); lua_pop(L, 1);

    // The library functions that loop in C, where the count hook never
    // fires, replaced by versions whose work counts against
    // rules.limits.instructions: the pattern functions (lua_pattern.c),
    // rep, move, and the table functions over plain tables only. The
    // string metatable's __index is this same table, so s:find() is the
    // counted one too.
    lua_getglobal(L, "string");
    const luaL_Reg patterns[] = {
        { "find",   budyk_lua_str_find   },
        { "match",  budyk_lua_str_match  },
        { "gmatch", budyk_lua_str_gmatch },
        { "gsub",   budyk_lua_str_gsub   },
        { nullptr,  nullptr },
    };
    luaL_setfuncs(L, patterns, 0);
    lua_getfield(L, -1, "rep");
    lua_pushcclosure(L, limited_rep, 1);
    lua_setfield(L, -2, "rep");
    lua_pop(L, 1);
    lua_getglobal(L, "table");
    lua_getfield(L, -1, "move");
    lua_pushcclosure(L, limited_move, 1);
    lua_setfield(L, -2, "move");
    for (const char* name : { "concat", "insert", "remove", "sort" }) {
        lua_getfield(L, -1, name);
        lua_pushstring(L, name);
        lua_pushcclosure(L, plain_table_only, 2);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);

    lua_pushcfunction(L, sandbox_xpcall);
    lua_setglobal(L, "xpcall");
    lua_getglobal(L, "setmetatable");
    lua_pushcclosure(L, sandbox_setmetatable, 1);
    lua_setglobal(L, "setmetatable");

    // Remove unsafe globals that `luaopen_base` also registered.
    const char* banned[] = { "dofile", "loadfile", "load", "loadstring", "require" };
    for (const char* name : banned) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
}

} // namespace

// The count hook fires every this many VM instructions. Small enough
// that an overrun is caught within a fraction of the limit, large
// enough that the hook costs nothing measurable (test_rule_perf).
constexpr int kHookInterval = 1000;

// The bytes the Lua state is handed count as instructions too, this many
// to one: a single instruction copies a string whatever its length
// (`s .. "x"` with s of 2 MiB, s:upper(), table.concat), and a rule well
// under the instruction limit took 25 s a tick that way. About what a
// copy of that many bytes costs next to a VM instruction.
constexpr size_t kAllocBytesPerInstruction = 64;

void* LuaEngine::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* self = static_cast<LuaEngine*>(ud);
    // When ptr is null, osize is the type of object being created, not
    // a size (Lua manual, lua_Alloc).
    const size_t old = ptr != nullptr ? osize : 0;

    if (nsize == 0) {
        std::free(ptr);
        self->memory_used_ -= old;
        return nullptr;
    }
    // Growth past the limit is refused; Lua then runs an emergency GC
    // and retries, and raises "not enough memory" if that fails too.
    // Shrinking must always succeed, so it is never refused.
    if (nsize > old && self->memory_used_ - old + nsize > self->memory_limit_) {
        return nullptr;
    }
    void* p = std::realloc(ptr, nsize);
    if (p == nullptr) return nullptr;
    self->memory_used_ = self->memory_used_ - old + nsize;
    // Growth is charged (kAllocBytesPerInstruction). An allocator must not
    // raise; past the limit the hook is set to fire on the next
    // instruction, and raises there (as lua.c stops a script from its
    // signal handler).
    if (nsize > old) {
        self->alloc_bytes_ += nsize - old;
        if (self->alloc_bytes_ >= kAllocBytesPerInstruction) {
            self->instructions_used_ += self->alloc_bytes_ / kAllocBytesPerInstruction;
            self->alloc_bytes_ %= kAllocBytesPerInstruction;
            if (self->instructions_used_ > self->instruction_limit_ && self->L_ != nullptr) {
                lua_sethook(self->L_, &LuaEngine::count_hook, LUA_MASKCOUNT, 1);
            }
        }
    }
    return p;
}

LuaEngine* LuaEngine::engine_of(lua_State* L) {
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    return static_cast<LuaEngine*>(ud);
}

int LuaEngine::raise_limit(lua_State* L, const LuaEngine* self) {
    // From here on the hook fires, and raises, on every instruction: a
    // pcall() or xpcall() in the rule catches the error only until its
    // next one, so the call ends however the rule nests them.
    lua_sethook(L, &LuaEngine::count_hook, LUA_MASKCOUNT, 1);
    return luaL_error(L, "instruction limit of %I exceeded (rules.limits.instructions)",
                      static_cast<lua_Integer>(self->instruction_limit_));
}

void LuaEngine::count_hook(lua_State* L, lua_Debug*) {
    auto* self = engine_of(L);
    self->instructions_used_ += kHookInterval;
    if (self->instructions_used_ > self->instruction_limit_) raise_limit(L, self);
}

void LuaEngine::charge(lua_State* L, uint64_t n) {
    auto* self = engine_of(L);
    const uint64_t room = self->instructions_used_ < self->instruction_limit_
                        ? self->instruction_limit_ - self->instructions_used_ : 0;
    if (n > room || self->instructions_used_ > self->instruction_limit_) {
        self->instructions_used_ = self->instruction_limit_ + 1;
        raise_limit(L, self);
    }
    self->instructions_used_ += n;
}

void LuaEngine::begin_call() {
    instructions_used_ = 0;
    alloc_bytes_       = 0;
    if (L_ != nullptr) lua_sethook(L_, &LuaEngine::count_hook, LUA_MASKCOUNT, kHookInterval);
}


int LuaEngine::init(bool enable_exec) {
    if (L_ != nullptr) return -1;
    exec_enabled_ = enable_exec;

    memory_used_ = 0;
    lua_State* L = lua_newstate(&LuaEngine::alloc, this);
    if (L == nullptr) return -2;
    lua_atpanic(L, on_panic);

    open_sandbox_libs(L);

    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, kEngineRegKey);

    budyk_lua_register_stdlib(L, enable_exec);
    budyk_lua_register_collected(L);

    lua_sethook(L, &LuaEngine::count_hook, LUA_MASKCOUNT, kHookInterval);

    L_ = L;
    return 0;
}

void LuaEngine::set_limits(uint64_t instructions, size_t memory_bytes) {
    instruction_limit_ = instructions;
    memory_limit_      = memory_bytes;
}

uint64_t LuaEngine::instruction_limit() const { return instruction_limit_; }
size_t   LuaEngine::memory_limit()      const { return memory_limit_; }
size_t   LuaEngine::memory_used()       const { return memory_used_; }

void LuaEngine::shutdown() {
    if (L_ == nullptr) return;
    for (const auto& r : rules_) {
        luaL_unref(L_, LUA_REGISTRYINDEX, r.when_ref);
        luaL_unref(L_, LUA_REGISTRYINDEX, r.actions_ref);
    }
    rules_.clear();
    for (const auto& c : level_conditions_) {
        luaL_unref(L_, LUA_REGISTRYINDEX, c.ref);
    }
    level_conditions_.clear();
    escalations_.clear();
    level_names_.clear();
    lua_close(L_);
    L_ = nullptr;
    last_fire_count_ = 0;
}

int LuaEngine::load_string(const char* code) {
    if (L_ == nullptr || code == nullptr) return -1;
    begin_call();
    loading_ = true;
    const int rc = luaL_dostring(L_, code);
    loading_ = false;
    if (rc != LUA_OK) {
        const char* msg = error_text(L_, -1);
        last_error_ = msg != nullptr ? msg : "unknown Lua error";
        lua_pop(L_, 1);
        return -2;
    }
    return 0;
}

int LuaEngine::load_file(const char* path) {
    if (L_ == nullptr || path == nullptr) return -1;
    begin_call();
    loading_ = true;
    const int rc = luaL_dofile(L_, path);
    loading_ = false;
    if (rc != LUA_OK) {
        const char* msg = error_text(L_, -1);
        last_error_ = msg != nullptr ? msg : "unknown Lua error";
        lua_pop(L_, 1);
        return -2;
    }
    return 0;
}

int LuaEngine::save_state(const char* path) const {
    if (path == nullptr) return -EINVAL;

    // Write to a sibling temp file then rename(2) over the target so a
    // crash mid-write can never leave a half-written state file.
    std::string tmp = path;
    tmp += ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "we");
    if (f == nullptr) return -errno;

    // Any failed write (a full disk) keeps the old file: a truncated one
    // renamed over it would reset every cooldown, the alert storm this
    // file is there to prevent.
    int e = 0;
    auto failed = [&e](bool bad) {
        if (bad && e == 0) e = errno != 0 ? errno : EIO;
    };
    errno = 0;
    failed(std::fputs("# budyk rule state v1\n", f) < 0);
    failed(std::fputs("# cooldown_remaining\tconsecutive_hits\tfire_count\tname\n", f) < 0);
    for (const auto& r : rules_) {
        // A newline in a rule name would corrupt the line-based format.
        // Such names are pathological (the name comes from watch()'s
        // first arg); skip persisting them rather than risk a mangled
        // file that breaks every subsequent rule's restore.
        if (r.name.find('\n') != std::string::npos) continue;
        failed(std::fprintf(f, "%d\t%d\t%llu\t%s\n",
                            r.cooldown_remaining, r.consecutive_hits,
                            static_cast<unsigned long long>(r.fire_count),
                            r.name.c_str()) < 0);
    }
    failed(std::fflush(f) != 0);
    failed(::fsync(::fileno(f)) != 0);
    failed(std::fclose(f) != 0);
    if (e != 0) {
        std::remove(tmp.c_str());
        return -e;
    }

    if (std::rename(tmp.c_str(), path) != 0) {
        e = errno;
        std::remove(tmp.c_str());
        return -e;
    }
    return 0;
}

int LuaEngine::load_state(const char* path) {
    if (path == nullptr) return -EINVAL;
    std::FILE* f = std::fopen(path, "re");
    if (f == nullptr) return 0;   // no state yet — fresh install, fine

    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char* p   = line;
        char* end = nullptr;

        const long cd = std::strtol(p, &end, 10);
        if (end == p || *end != '\t') continue;
        p = end + 1;
        const long ch = std::strtol(p, &end, 10);
        if (end == p || *end != '\t') continue;
        p = end + 1;
        const unsigned long long fc = std::strtoull(p, &end, 10);
        if (end == p || *end != '\t') continue;
        p = end + 1;

        std::string name(p);
        while (!name.empty() &&
               (name.back() == '\n' || name.back() == '\r')) {
            name.pop_back();
        }
        if (name.empty()) continue;

        for (auto& r : rules_) {
            if (r.name == name) {
                r.cooldown_remaining = static_cast<int>(cd);
                r.consecutive_hits   = static_cast<int>(ch);
                r.fire_count         = fc;
                break;
            }
        }
    }
    std::fclose(f);
    return 0;
}

bool LuaEngine::bind(const Sample& s, const budyk_detail_c* detail) {
    begin_call();
    if (budyk_lua_bind(L_, s, detail, has_file_state_ ? &file_state_ : nullptr) == LUA_OK) {
        bind_error_.clear();
        return true;
    }
    // A failed allocation, as a rule that holds the state at the memory
    // limit makes it: the tick goes without rules, logged once.
    const char* m = error_text(L_, -1);
    const std::string msg = m != nullptr ? m : "unknown error";
    lua_pop(L_, 1);
    if (bind_error_ != msg) {
        bind_error_ = msg;
        std::fprintf(stderr, "budyk: rules not evaluated: binding the sample failed: %s\n",
                     msg.c_str());
    }
    return false;
}

int LuaEngine::eval_tick(const Sample& s, const budyk_detail_c* detail) {
    if (L_ == nullptr) return -1;

    last_fire_count_ = 0;
    if (!bind(s, detail)) return 0;

    for (auto& r : rules_) {
        if (r.cooldown_remaining > 0) {
            --r.cooldown_remaining;
            continue;
        }

        const int base = lua_gettop(L_);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, r.when_ref);
        begin_call();
        if (lua_pcall(L_, 0, LUA_MULTRET, 0) != LUA_OK) {
            if (const int why = budyk_lua_uncollected(L_, -1); why != 0) {
                // Skipped: the sustain count stays. A rate's first read
                // passes in a tick; a level that does not collect it is
                // worth a line.
                if (why == 1) {
                    log_skip(&r.last_skip, ("rule '" + r.name + "'").c_str(), error_text(L_, -1));
                }
                lua_pop(L_, 1);
                continue;
            }
            log_rule_error(&r.last_error, r.name.c_str(), "when", error_text(L_, -1));
            lua_pop(L_, 1);
            r.consecutive_hits = 0;
            continue;
        }
        // Spec §3.6.5: nil means "cannot tell on this tick" (a device that
        // is not there, say: `return disk.devices.sda and ...`) and skips
        // the rule, its sustain count kept. A when() that returns nothing
        // at all (an `if` without an else) is false.
        const int  nret = lua_gettop(L_) - base;
        const bool skip = nret > 0 && lua_isnil(L_, base + 1);
        const bool hit  = nret > 0 && lua_toboolean(L_, base + 1) != 0;
        lua_settop(L_, base);
        r.last_error.clear();

        if (skip) continue;
        if (!hit) {
            r.consecutive_hits = 0;
            continue;
        }

        ++r.consecutive_hits;
        if (r.consecutive_hits < r.for_ticks) continue;

        ++last_fire_count_;
        ++r.fire_count;
        r.consecutive_hits   = 0;
        r.cooldown_remaining = r.cooldown_ticks;

        run_actions(&r);
    }
    return last_fire_count_;
}

void LuaEngine::run_actions(LuaRule* r) {
    // Every action runs, in order: one that fails is logged and the next
    // still runs (an alert must not depend on a script before it).
    lua_rawgeti(L_, LUA_REGISTRYINDEX, r->actions_ref);
    const int list = lua_gettop(L_);
    const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L_, list));
    for (lua_Integer i = 1; i <= n; ++i) {
        if (lua_rawgeti(L_, list, i) == LUA_TFUNCTION) {
            begin_call();
            if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
                log_rule_error(&r->last_error, r->name.c_str(), "action", error_text(L_, -1));
                lua_pop(L_, 1);
            }
            continue;
        }
        const char* tag = lua_tostring(L_, -1);
        const std::string what = tag != nullptr ? tag : "";
        lua_pop(L_, 1);
        if (what == "alert") {
            raise_alert(r->severity, r->name, r->message);
        } else if (what == "log") {
            // Same stream and format as Lua's print() / the YAML "log" action.
            std::printf("[budyk] %s\n", r->message.c_str());
            std::fflush(stdout);
        } else if (what == "escalate") {
            push_escalation("L3", 60);   // spec §3.6.2: { alert, escalate }
        }
    }
    lua_settop(L_, list - 1);
}

int LuaEngine::raise_alert(AlertSeverity sev, const std::string& rule, const std::string& message) {
    std::fprintf(stderr, "budyk alert: %s %s: %s\n", severity_name(sev), rule.c_str(),
                 message.c_str());
    if (alert_events_.size() < kMaxPendingAlerts) {
        alert_events_.push_back(AlertEvent{now_realtime_ns(), severity_name(sev), rule, message});
    } else {
        ++alerts_dropped_;
    }
    return alerts_.dispatch(sev, rule, message);
}

std::vector<AlertEvent> LuaEngine::take_alerts() {
    std::vector<AlertEvent> out;
    out.swap(alert_events_);
    return out;
}

uint64_t LuaEngine::alerts_dropped() const { return alerts_dropped_; }
bool     LuaEngine::loading()        const { return loading_; }

int  LuaEngine::rule_count()      const { return static_cast<int>(rules_.size()); }
const std::string& LuaEngine::last_error() const { return last_error_; }
int  LuaEngine::last_fire_count() const { return last_fire_count_; }
bool LuaEngine::exec_enabled()    const { return exec_enabled_; }
bool LuaEngine::freeze_enabled()  const { return freeze_enabled_; }

const std::vector<LuaRule>& LuaEngine::rules() const { return rules_; }

void LuaEngine::set_exec_allowlist(std::vector<std::string> paths) {
    exec_allowlist_ = std::move(paths);
    exec_allowlist_resolved_.clear();
    for (const auto& p : exec_allowlist_) {
        char buf[PATH_MAX];
        const char* r = ::realpath(p.c_str(), buf);
        exec_allowlist_resolved_.push_back(r != nullptr ? std::string(r) : p);
    }
}

const std::vector<std::string>& LuaEngine::exec_allowlist_resolved() const {
    return exec_allowlist_resolved_;
}

const std::vector<std::string>& LuaEngine::exec_allowlist() const {
    return exec_allowlist_;
}

void LuaEngine::set_freeze_enabled(bool enable) {
    freeze_enabled_ = enable;
}

void LuaEngine::set_freeze_allowlist(std::vector<std::string> names) {
    freeze_allowlist_ = std::move(names);
}

const std::vector<std::string>& LuaEngine::freeze_allowlist() const {
    return freeze_allowlist_;
}

AlertDispatcher&       LuaEngine::alerts()       { return alerts_; }
const AlertDispatcher& LuaEngine::alerts() const { return alerts_; }
Worker&                LuaEngine::exec_worker()  { return exec_worker_; }

void LuaEngine::set_file_state(const FileWatchState& s) {
    file_state_     = s;
    has_file_state_ = true;
}

bool                  LuaEngine::has_file_state() const { return has_file_state_; }
const FileWatchState& LuaEngine::file_state()     const { return file_state_; }

int LuaEngine::add_level_condition(uint8_t level_id, const std::string& expr) {
    if (L_ == nullptr) return -1;
    const std::string chunk = "return (" + expr + ")";
    const std::string name  = "=level " + std::to_string(level_id) + " when";
    if (luaL_loadbuffer(L_, chunk.data(), chunk.size(), name.c_str()) != LUA_OK) {
        const char* msg = error_text(L_, -1);
        last_error_ = msg != nullptr ? msg : "unknown Lua error";
        lua_pop(L_, 1);
        return -2;
    }
    level_conditions_.push_back(
        LevelCondition{level_id, luaL_ref(L_, LUA_REGISTRYINDEX), std::string(), std::string()});
    return 0;
}

void LuaEngine::eval_level_conditions(const Sample& s, std::vector<uint8_t>* active,
                                      const budyk_detail_c* detail) {
    if (active == nullptr) return;
    active->clear();
    if (L_ == nullptr || level_conditions_.empty()) return;

    if (!bind(s, detail)) return;
    for (auto& c : level_conditions_) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, c.ref);
        begin_call();
        if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
            if (const int why = budyk_lua_uncollected(L_, -1); why != 0) {
                if (why == 1) {
                    const std::string what = "level " + std::to_string(c.level_id) + " condition";
                    log_skip(&c.last_skip, what.c_str(), error_text(L_, -1));
                }
                lua_pop(L_, 1);
                continue;
            }
            const char* m = error_text(L_, -1);
            const std::string msg = m != nullptr ? m : "unknown error";
            if (c.last_error != msg) {
                c.last_error = msg;
                std::fprintf(stderr, "budyk: level %u condition failed: %s\n",
                             static_cast<unsigned>(c.level_id), msg.c_str());
            }
            lua_pop(L_, 1);
            continue;
        }
        c.last_error.clear();
        if (lua_toboolean(L_, -1) != 0) active->push_back(c.level_id);
        lua_pop(L_, 1);
    }
}

const std::vector<LuaEngine::LevelCondition>& LuaEngine::level_conditions() const {
    return level_conditions_;
}

void LuaEngine::set_level_names(std::vector<std::string> names) {
    level_names_ = std::move(names);
}

bool LuaEngine::is_level_name(const char* name) const {
    if (name == nullptr) return false;
    for (const auto& n : level_names_) {
        if (::strcasecmp(n.c_str(), name) == 0) return true;
    }
    return false;
}

void LuaEngine::push_escalation(const char* level, int seconds) {
    escalations_.push_back(Escalation{level != nullptr ? level : "", seconds});
}

std::vector<Escalation> LuaEngine::take_escalations() {
    std::vector<Escalation> out;
    out.swap(escalations_);
    return out;
}

void LuaEngine::add_rule(const std::string& name, int when_ref, int actions_ref,
                         AlertSeverity severity, const std::string& message,
                         int for_ticks, int cooldown_ticks) {
    if (for_ticks      < 1) for_ticks      = 1;
    if (cooldown_ticks < 0) cooldown_ticks = 0;
    rules_.push_back(LuaRule{
        name, when_ref, actions_ref, severity, message,
        /*fire_count*/ 0,
        for_ticks, cooldown_ticks,
        /*consecutive_hits*/ 0, /*cooldown_remaining*/ 0,
        /*last_error*/ std::string(), /*last_skip*/ std::string()
    });
}

} // namespace budyk

extern "C" void budyk_lua_charge(lua_State* L, size_t n) {
    budyk::LuaEngine::charge(L, n);
}
