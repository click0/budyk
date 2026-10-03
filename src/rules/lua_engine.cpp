// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_engine.h"

#include "rules/lua_bindings.h"
#include "rules/lua_stdlib.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cerrno>
#include <strings.h>
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

void open_sandbox_libs(lua_State* L) {
    // Only the safe subset: base + math + string + table.
    luaL_requiref(L, "_G",     luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, "math",   luaopen_math,   1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "table",  luaopen_table,  1); lua_pop(L, 1);

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
    return p;
}

void LuaEngine::count_hook(lua_State* L, lua_Debug*) {
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    auto* self = static_cast<LuaEngine*>(ud);
    self->instructions_used_ += kHookInterval;
    if (self->instructions_used_ > self->instruction_limit_) {
        luaL_error(L, "instruction limit of %I exceeded (rules.limits.instructions)",
                   static_cast<lua_Integer>(self->instruction_limit_));
    }
}

void LuaEngine::begin_call() { instructions_used_ = 0; }

int LuaEngine::init(bool enable_exec) {
    if (L_ != nullptr) return -1;
    exec_enabled_ = enable_exec;

    memory_used_ = 0;
    lua_State* L = lua_newstate(&LuaEngine::alloc, this);
    if (L == nullptr) return -2;

    open_sandbox_libs(L);

    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, kEngineRegKey);

    budyk_lua_register_stdlib(L, enable_exec);

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
        if (r.action_ref != LUA_REFNIL) {
            luaL_unref(L_, LUA_REGISTRYINDEX, r.action_ref);
        }
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
    if (luaL_dostring(L_, code) != LUA_OK) {
        const char* msg = lua_tostring(L_, -1);
        last_error_ = msg != nullptr ? msg : "unknown Lua error";
        lua_pop(L_, 1);
        return -2;
    }
    return 0;
}

int LuaEngine::load_file(const char* path) {
    if (L_ == nullptr || path == nullptr) return -1;
    begin_call();
    if (luaL_dofile(L_, path) != LUA_OK) {
        const char* msg = lua_tostring(L_, -1);
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
    std::FILE* f = std::fopen(tmp.c_str(), "w");
    if (f == nullptr) return -errno;

    std::fputs("# budyk rule state v1\n", f);
    std::fputs("# cooldown_remaining\tconsecutive_hits\tfire_count\tname\n", f);
    for (const auto& r : rules_) {
        // A newline in a rule name would corrupt the line-based format.
        // Such names are pathological (the name comes from watch()'s
        // first arg); skip persisting them rather than risk a mangled
        // file that breaks every subsequent rule's restore.
        if (r.name.find('\n') != std::string::npos) continue;
        std::fprintf(f, "%d\t%d\t%llu\t%s\n",
                     r.cooldown_remaining, r.consecutive_hits,
                     static_cast<unsigned long long>(r.fire_count),
                     r.name.c_str());
    }
    std::fflush(f);
    std::fclose(f);

    if (std::rename(tmp.c_str(), path) != 0) {
        const int e = -errno;
        std::remove(tmp.c_str());
        return e;
    }
    return 0;
}

int LuaEngine::load_state(const char* path) {
    if (path == nullptr) return -EINVAL;
    std::FILE* f = std::fopen(path, "r");
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

int LuaEngine::eval_tick(const Sample& s) {
    if (L_ == nullptr) return -1;

    budyk_lua_bind_sample(L_, s);
    if (has_file_state_) {
        budyk_lua_bind_files(L_, file_state_);
    }
    last_fire_count_ = 0;

    for (auto& r : rules_) {
        if (r.cooldown_remaining > 0) {
            --r.cooldown_remaining;
            continue;
        }

        lua_rawgeti(L_, LUA_REGISTRYINDEX, r.when_ref);
        begin_call();
        if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
            log_rule_error(&r.last_error, r.name.c_str(), "when", lua_tostring(L_, -1));
            lua_pop(L_, 1);
            r.consecutive_hits = 0;
            continue;
        }
        const bool hit = lua_toboolean(L_, -1) != 0;
        lua_pop(L_, 1);
        r.last_error.clear();

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

        if (r.action_ref != LUA_REFNIL) {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, r.action_ref);
            begin_call();
            if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
                log_rule_error(&r.last_error, r.name.c_str(), "action", lua_tostring(L_, -1));
                lua_pop(L_, 1);
            }
        } else if (r.action_tag == "alert") {
            alerts_.dispatch(r.severity, r.name, r.message);
        } else if (r.action_tag == "log") {
            // Same stream and format as Lua's print() / the YAML "log" action.
            std::printf("[budyk] %s\n", r.message.c_str());
            std::fflush(stdout);
        }
    }
    return last_fire_count_;
}

int  LuaEngine::rule_count()      const { return static_cast<int>(rules_.size()); }
const std::string& LuaEngine::last_error() const { return last_error_; }
int  LuaEngine::last_fire_count() const { return last_fire_count_; }
bool LuaEngine::exec_enabled()    const { return exec_enabled_; }
bool LuaEngine::freeze_enabled()  const { return freeze_enabled_; }

const std::vector<LuaRule>& LuaEngine::rules() const { return rules_; }

void LuaEngine::set_exec_allowlist(std::vector<std::string> paths) {
    exec_allowlist_ = std::move(paths);
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
        const char* msg = lua_tostring(L_, -1);
        last_error_ = msg != nullptr ? msg : "unknown Lua error";
        lua_pop(L_, 1);
        return -2;
    }
    level_conditions_.push_back(
        LevelCondition{level_id, luaL_ref(L_, LUA_REGISTRYINDEX), std::string()});
    return 0;
}

void LuaEngine::eval_level_conditions(const Sample& s, std::vector<uint8_t>* active) {
    if (active == nullptr) return;
    active->clear();
    if (L_ == nullptr || level_conditions_.empty()) return;

    budyk_lua_bind_sample(L_, s);
    if (has_file_state_) {
        budyk_lua_bind_files(L_, file_state_);
    }
    for (auto& c : level_conditions_) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, c.ref);
        begin_call();
        if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
            const char* m = lua_tostring(L_, -1);
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

void LuaEngine::add_rule(const std::string& name, int when_ref, int action_ref,
                         const std::string& action_tag,
                         AlertSeverity severity, const std::string& message,
                         int for_ticks, int cooldown_ticks) {
    if (for_ticks      < 1) for_ticks      = 1;
    if (cooldown_ticks < 0) cooldown_ticks = 0;   // default: no cooldown
    rules_.push_back(LuaRule{
        name, when_ref, action_ref, action_tag, severity, message,
        /*fire_count*/ 0,
        for_ticks, cooldown_ticks,
        /*consecutive_hits*/ 0, /*cooldown_remaining*/ 0,
        /*last_error*/ std::string()
    });
}

} // namespace budyk
