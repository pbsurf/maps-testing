#include "counters.h"

#include <dirent.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace BenchCounters {

struct Event { const char* name; uint32_t type; uint64_t config; };

static const Event EVENTS[] = {
    {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},  // group leader
    {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
    {"branch_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
    {"llc_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},  // last level cache misses on Intel
    {"l1d_misses", PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                                       (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)},
};
static constexpr int NEVENTS = sizeof(EVENTS)/sizeof(EVENTS[0]);

struct ReadFormat { uint64_t nr, timeEnabled, timeRunning, values[NEVENTS]; };

struct ThreadCounters {
    pid_t tid;
    std::string group;
    std::vector<int> fds;
    uint64_t base[NEVENTS] = {};  // values at the last begin(); 0 if opened later
};

static std::mutex mutex;
static std::vector<ThreadCounters> threads;
static std::set<pid_t> seen;
static bool isEnabled = false;
static bool multiplexed = false;

static pid_t gettid_() { return pid_t(syscall(SYS_gettid)); }

static bool openThread(pid_t tid, const std::string& group) {
    ThreadCounters tc;
    tc.tid = tid;
    tc.group = group;
    for (int i = 0; i < NEVENTS; ++i) {
        perf_event_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.size = sizeof(attr);
        attr.type = EVENTS[i].type;
        attr.config = EVENTS[i].config;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
        int leader = tc.fds.empty() ? -1 : tc.fds[0];
        int fd = int(syscall(SYS_perf_event_open, &attr, tid, -1, leader, PERF_FLAG_FD_CLOEXEC));
        if (fd < 0) {
            fprintf(stderr, "counters: perf_event_open(%s) for thread %d failed: %s\n",
                    EVENTS[i].name, int(tid), strerror(errno));
            for (int f : tc.fds) { close(f); }
            return false;
        }
        tc.fds.push_back(fd);
    }
    seen.insert(tid);
    threads.push_back(std::move(tc));
    return true;
}

static bool readThread(const ThreadCounters& tc, uint64_t* out) {
    ReadFormat rf;
    if (read(tc.fds[0], &rf, sizeof(rf)) != ssize_t(sizeof(rf)) || rf.nr != NEVENTS) { return false; }
    if (rf.timeRunning < rf.timeEnabled) { multiplexed = true; }
    for (int i = 0; i < NEVENTS; ++i) { out[i] = rf.values[i]; }
    return true;
}

bool enable() {
    std::lock_guard<std::mutex> lock(mutex);
    isEnabled = openThread(gettid_(), "main");
    return isEnabled;
}

bool enabled() { return isEnabled; }

void addCurrentThread(const char* group) {
    if (!isEnabled) { return; }
    std::lock_guard<std::mutex> lock(mutex);
    pid_t tid = gettid_();
    if (!seen.count(tid)) { openThread(tid, group); }
}

void begin() {
    if (!isEnabled) { return; }
    std::lock_guard<std::mutex> lock(mutex);
    // threads started since the last phase that didn't register themselves (mbtiles, async workers)
    if (DIR* dir = opendir("/proc/self/task")) {
        while (dirent* e = readdir(dir)) {
            pid_t tid = pid_t(atoi(e->d_name));
            if (tid > 0 && !seen.count(tid)) { openThread(tid, "other"); }
        }
        closedir(dir);
    }
    for (auto& tc : threads) {
        readThread(tc, tc.base);  // on failure (exited thread) keep the base from the last endJson()
    }
    multiplexed = false;
}

std::string endJson() {
    if (!isEnabled) { return "null"; }
    std::lock_guard<std::mutex> lock(mutex);
    std::map<std::string, std::vector<uint64_t>> sums;
    for (const char* g : {"main", "tile_worker", "other"}) { sums[g].assign(NEVENTS, 0); }
    for (auto& tc : threads) {
        uint64_t v[NEVENTS];
        // a thread that exited keeps its final counts
        if (!readThread(tc, v)) { continue; }
        auto& s = sums[tc.group];
        for (int i = 0; i < NEVENTS; ++i) { s[i] += v[i] - tc.base[i]; }
        // counted once: the next phase measures from here
        memcpy(tc.base, v, sizeof(v));
    }
    std::string json = "{";
    char buf[64];
    for (auto& g : sums) {
        json += (json.size() > 1 ? ", \"" : "\"") + g.first + "\": {";
        for (int i = 0; i < NEVENTS; ++i) {
            snprintf(buf, sizeof(buf), "%s\"%s\": %llu", i ? ", " : "", EVENTS[i].name,
                     (unsigned long long)g.second[i]);
            json += buf;
        }
        json += "}";
    }
    json += std::string(", \"multiplexed\": ") + (multiplexed ? "true" : "false") + "}";
    return json;
}

}
