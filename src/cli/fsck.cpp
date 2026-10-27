// SPDX-License-Identifier: BSD-3-Clause
// budyk fsck [--repair] [--config FILE] [DATA_DIR]
//
// Checks every ring file in the data directory (tier1..3.ring and the
// level-<name>.ring files): the header, then each record write_idx
// counts (CRC, level, time order). --repair counts in the records that
// were written past write_idx but never indexed, the one thing a crash
// leaves that can be put right; it takes the data_dir lock, so the
// daemon must be stopped. Exit status, as fsck(8) has it: 0 clean,
// 1 problems found and all repaired, 4 problems left, 8 the check could
// not run (usage, config, data_dir, lock).

#include "cli/commands.h"

#include "config/config.h"
#include "core/clock.h"
#include "storage/codec.h"
#include "storage/ring_check.h"
#include "util/dir_lock.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace budyk {

namespace {

constexpr int kExitClean    = 0;
constexpr int kExitRepaired = 1;
constexpr int kExitLeft     = 4;
constexpr int kExitFailed   = 8;

std::string utc(uint64_t ns) {
    const time_t t = static_cast<time_t>(ns / 1000000000ULL);
    struct tm tm{};
    char buf[32] = "?";
    if (::gmtime_r(&t, &tm) != nullptr) std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

const char* ring_level(uint8_t tier) {
    switch (tier) {
        case 1: return "L3";
        case 2: return "L2";
        case 3: return "L1";
        default: return "custom level";
    }
}

// tier1..3.ring, then the level-*.ring files in name order.
std::vector<std::string> ring_files(const char* dir) {
    std::vector<std::string> names = {"tier1.ring", "tier2.ring", "tier3.ring"};
    std::vector<std::string> custom;
    if (DIR* d = ::opendir(dir)) {
        while (const dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 11 && n.compare(0, 6, "level-") == 0 &&
                n.compare(n.size() - 5, 5, ".ring") == 0) {
                custom.push_back(n);
            }
        }
        ::closedir(d);
    }
    std::sort(custom.begin(), custom.end());
    names.insert(names.end(), custom.begin(), custom.end());
    return names;
}

// Prints what check_ring() found for one ring; true if it found an error
// this run did not repair.
bool report(const std::string& name, const RingCheck& c, bool repair) {
    const RingFile::Header& h = c.header;
    if (c.header_rc != 0) {
        std::printf("%s: %s%s%s: not checked; move it aside and budyk serve starts a new one\n",
                    name.c_str(), RingFile::describe(c.header_rc),
                    c.header_errno != 0 ? ": " : "",
                    c.header_errno != 0 ? std::strerror(c.header_errno) : "");
        return true;
    }
    if (c.unknown_tier) {
        std::printf("%s: unknown tier byte 0x%02x in the header: not checked\n",
                    name.c_str(), h.tier);
        return true;
    }
    if (c.older_format) {
        std::printf("%s: %u-byte records, from an older budyk (this one writes %zu): "
                    "budyk serve converts it on start; not checked\n",
                    name.c_str(), h.record_size, record_size_for_sample());
        return false;
    }
    if (c.newer_format) {
        std::printf("%s: %u-byte records, from a newer budyk (this one writes %zu): "
                    "not checked\n", name.c_str(), h.record_size, record_size_for_sample());
        return true;
    }
    if (c.open_rc != 0) {
        std::printf("%s: %s: not checked\n", name.c_str(), RingFile::describe(c.open_rc));
        return true;
    }

    std::printf("%s: %llu record%s (%s", name.c_str(),
                static_cast<unsigned long long>(c.records), c.records == 1 ? "" : "s",
                ring_level(h.tier));
    if (c.folded > 0) std::printf(", %llu folded", static_cast<unsigned long long>(c.folded));
    std::printf(")");
    if (c.valid > 0) std::printf(", %s .. %s UTC", utc(c.first_ts).c_str(), utc(c.last_ts).c_str());
    const bool clean = !c.errors() && c.recovered == 0 && c.out_of_order == 0 && c.future == 0;
    std::printf("%s\n", clean ? ": ok" : "");

    if (c.damaged > 0) {
        std::printf("  %llu damaged record%s (slot", static_cast<unsigned long long>(c.damaged),
                    c.damaged == 1 ? "" : "s");
        for (size_t i = 0; i < c.damaged_slots.size(); ++i) {
            std::printf("%s %llu", i == 0 ? (c.damaged == 1 ? "" : "s") : ",",
                        static_cast<unsigned long long>(c.damaged_slots[i]));
        }
        std::printf("%s): skipped on read, cannot be repaired\n",
                    c.damaged > c.damaged_slots.size() ? ", ..." : "");
    }
    if (c.read_errors > 0) {
        std::printf("  %llu record%s could not be read (I/O error)\n",
                    static_cast<unsigned long long>(c.read_errors), c.read_errors == 1 ? "" : "s");
    }
    if (c.wrong_level > 0) {
        std::printf("  %llu record%s of another level than this ring's\n",
                    static_cast<unsigned long long>(c.wrong_level), c.wrong_level == 1 ? "" : "s");
    }
    if (c.out_of_order > 0) {
        std::printf("  note: %llu record%s older than one before %s (the clock stepped "
                    "back?): a query across that point may miss records\n",
                    static_cast<unsigned long long>(c.out_of_order),
                    c.out_of_order == 1 ? "" : "s", c.out_of_order == 1 ? "it" : "them");
    }
    if (c.future > 0) {
        std::printf("  note: %llu record%s more than a day ahead of this host's clock\n",
                    static_cast<unsigned long long>(c.future), c.future == 1 ? "" : "s");
    }
    if (c.unindexed > 0) {
        std::printf("  %llu record%s written past write_idx and never counted (a crash "
                    "before the index update): %s\n",
                    static_cast<unsigned long long>(c.unindexed), c.unindexed == 1 ? "" : "s",
                    c.recovered > 0 ? "counted in"
                                    : repair ? "could not be counted in"
                                             : "budyk fsck --repair, or the next start, counts it in");
    }
    return c.errors();
}

} // namespace

int cmd_fsck(int argc, char* argv[]) {
    const char* config_path = default_config_path();
    const char* dir         = nullptr;
    bool        repair      = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--repair") == 0) {
            repair = true;
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (argv[i][0] != '-' && dir == nullptr) {
            dir = argv[i];
        } else {
            std::fprintf(stderr,
                "budyk fsck: unknown arg '%s'\n"
                "usage: budyk fsck [--repair] [--config FILE] [DATA_DIR]\n", argv[i]);
            return kExitFailed;
        }
    }

    Config cfg;
    if (dir == nullptr) {
        std::string err;
        if (config_load(config_path, &cfg, &err) != 0) {
            std::fprintf(stderr,
                "budyk fsck: failed to load config '%s': %s (or name the data_dir)\n",
                config_path, err.c_str());
            return kExitFailed;
        }
        dir = cfg.data_dir;
    }
    struct stat st{};
    if (::stat(dir, &st) != 0) {
        std::fprintf(stderr, "budyk fsck: data_dir '%s': %s\n", dir, std::strerror(errno));
        return kExitFailed;
    }
    if (!S_ISDIR(st.st_mode)) {
        std::fprintf(stderr, "budyk fsck: data_dir '%s' is not a directory\n", dir);
        return kExitFailed;
    }

    // A repair writes the headers: no daemon may be running. A check only
    // reads, as the API does, and goes ahead.
    int lock_fd = -1;
    std::string holder;
    if (repair) {
        lock_fd = lock_data_dir(dir, &holder);
        if (lock_fd == -EWOULDBLOCK) {
            std::fprintf(stderr,
                "budyk fsck: '%s' is in use by a running budyk (pid %s): stop it before "
                "--repair\n", dir, holder.empty() ? "?" : holder.c_str());
            return kExitFailed;
        }
        if (lock_fd < 0) {
            std::fprintf(stderr, "budyk fsck: cannot lock '%s/budyk.lock': %s\n", dir,
                         std::strerror(-lock_fd));
            return kExitFailed;
        }
    } else if (data_dir_locked(dir, &holder)) {
        std::printf("note: a running budyk (pid %s) writes to %s; its newest records may "
                    "change while they are read\n", holder.empty() ? "?" : holder.c_str(), dir);
    }

    const uint64_t now = now_realtime_ns();
    int checked = 0, left = 0, repaired = 0;
    for (const auto& name : ring_files(dir)) {
        const std::string path = std::string(dir) + "/" + name;
        if (::access(path.c_str(), F_OK) != 0) {
            std::printf("%s: missing (budyk serve creates it)\n", name.c_str());
            continue;
        }
        RingCheck c;
        const int rc = check_ring(path.c_str(), repair, now, &c);
        ++checked;
        if (rc != 0) {
            std::printf("%s: cannot open it for the repair: %s\n", name.c_str(), std::strerror(-rc));
            ++left;
            continue;
        }
        if (report(name, c, repair)) ++left;
        if (c.recovered > 0) ++repaired;
    }
    if (lock_fd >= 0) ::close(lock_fd);

    std::printf("budyk fsck: %s: %d ring%s checked, %d repaired, %d with problems left\n",
                dir, checked, checked == 1 ? "" : "s", repaired, left);
    if (left > 0)     return kExitLeft;
    if (repaired > 0) return kExitRepaired;
    return kExitClean;
}

} // namespace budyk
