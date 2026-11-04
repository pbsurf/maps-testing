#pragma once
// Exact per-thread user-space hardware counters (perf_event_open), read at phase boundaries (--counters,
//  used by bench.py perf). Each thread gets one counter group; threads are grouped as main, tile_worker
//  and other for reporting.

#include <string>

namespace BenchCounters {

// open counters for the calling thread (the main thread); false if perf_event_open fails
bool enable();
bool enabled();
// open counters for the calling thread, if enabled (tile workers call this when they start)
void addCurrentThread(const char* group);
// phase start: add counters for threads not seen yet (as "other"), and take every thread's baseline
void begin();
// phase end: JSON object with per-group counts since begin(), e.g. {"main": {"instructions": N, ...}, ...}
std::string endJson();

}
