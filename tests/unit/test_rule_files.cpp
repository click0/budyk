// SPDX-License-Identifier: BSD-3-Clause
// The rule files shipped in rules/ load and run: every rule registers,
// and a tick on a full sample (and one on a sample without the disk
// group) raises no error in any rule.
//
//   test_rule_files <path to rules/>
#include "core/metric_set.h"
#include "core/sample.h"
#include "rules/lua_engine.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

using namespace budyk;

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string dir = argv[1];

    Sample s{};
    s.groups = kGroupsAll;
    s.level  = Level::L3;
    s.timestamp_nanos   = 1;
    s.cpu.total_percent = 99.0;
    s.cpu.count         = 4;
    s.mem.total         = 16ULL << 30;
    s.mem.available     = 1ULL << 20;
    s.mem.available_percent = 1.0;
    s.swap.used_percent = 95.0;
    s.load.avg_1m       = 40.0;
    s.disk.write_bytes_per_sec = 600ULL << 20;
    std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
    d->collected = kDetailAll;
    d->ndisks = 1;
    std::snprintf(d->disks[0].name, sizeof(d->disks[0].name), "sda");
    d->disks[0].busy_percent = 99.0;

    Sample bare = s;
    clear_groups(&bare, kGroupDisk);

    for (const char* file : { "examples.lua", "freebsd-defaults.lua" }) {
        LuaEngine e;
        assert(e.init(false) == 0);
        e.set_level_names({"L1", "L2", "L3"});
        const std::string path = dir + "/" + file;
        if (e.load_file(path.c_str()) != 0) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), e.last_error().c_str());
            assert(false);
        }
        assert(e.rule_count() >= 3);
        for (int t = 0; t < 30; ++t) {
            e.eval_tick(t % 3 == 2 ? bare : s, d.get());
            for (const auto& r : e.rules()) {
                if (!r.last_error.empty()) {
                    std::fprintf(stderr, "%s: rule '%s': %s\n", file, r.name.c_str(),
                                 r.last_error.c_str());
                    assert(false);
                }
            }
        }
        for (const auto& r : e.rules()) assert(r.fire_count >= 1);   // every one fired
        e.shutdown();
    }

    std::printf("test_rule_files: PASS\n");
    return 0;
}
