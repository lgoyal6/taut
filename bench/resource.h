// Errors, memory and cost for the latency benchmarks (C02).
//
// The three things a latency table never says on its own: what failed while the numbers
// were being produced, how much memory the process held to produce them, and what the
// work cost. A p999 next to an unreported 4% delivery failure is not a measurement of the
// transport, it is a measurement of the messages that happened to arrive.
//
// Kept out of common.h on purpose: common.h is the wire format and the schedule, shared by
// the taut, TCP and ENet binaries and documented in docs/BENCHMARKS.md. This is process
// accounting, and it is written to its own CSV beside the latency CSV so the documented
// schemas do not change.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace bench {

// Peak resident set of this process, in bytes.
//
// ru_maxrss is BYTES on Darwin and KILOBYTES on Linux. That is a portability trap worth
// naming rather than a factor of 1024 worth guessing, and getting it wrong silently
// produces a memory number that is wrong by three orders of magnitude in the flattering
// direction on exactly one of the two platforms this benchmark runs on.
inline std::uint64_t peak_rss_bytes() {
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);
#else
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024ULL;
#endif
}

// Resident set right now. Reported next to the peak so a transient spike (a reserve() of
// the sample vector, a burst of retransmit buffers) can be told apart from a working set
// that stays resident for the whole run.
inline std::uint64_t current_rss_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        return static_cast<std::uint64_t>(info.resident_size);
    }
    return 0;
#else
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) {
        return 0;
    }
    unsigned long total = 0, resident = 0;
    const int got = std::fscanf(f, "%lu %lu", &total, &resident);
    std::fclose(f);
    return got == 2 ? static_cast<std::uint64_t>(resident) * 4096ULL : 0;
#endif
}

// User + system CPU seconds charged to this process.
//
// This is the cost axis, and it is the one that matters for a poll/tick transport with no
// event-loop driver: both roles spin, so wall time says how long the run lasted and CPU
// time says what it burned to last that long. They are not the same number, and on a
// machine shared with other work the difference is the whole story.
inline double cpu_seconds() {
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<double>(ru.ru_utime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec) / 1e6 +
           static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
}

// Voluntary + involuntary context switches. An involuntary switch is the kernel taking the
// CPU away, which is what contention from other processes on the box looks like from
// inside the benchmark. Reported so a tail that came from a busy machine can be told apart
// from a tail that came from the transport.
inline void context_switches(std::uint64_t& voluntary, std::uint64_t& involuntary) {
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    voluntary = static_cast<std::uint64_t>(ru.ru_nvcsw);
    involuntary = static_cast<std::uint64_t>(ru.ru_nivcsw);
}

// Price of one CPU-second, printed alongside every derived dollar figure.
//
// USD is arithmetic on a measured quantity, not a measurement. The measured quantity is
// cpu_s; publishing the rate with it means the figure can be redone against a different
// instance or a different price without re-running anything.
inline constexpr double kUsdPerCpuSecond = 0.145 / 4.0 / 3600.0; // c7g.xlarge on-demand / 4 vCPU
inline constexpr const char* kRateLabel = "c7g.xlarge_0.145usd_hr_4vcpu";

struct Resources {
    std::uint64_t rss_peak = 0;
    std::uint64_t rss_steady = 0;
    double cpu_s = 0;
    std::uint64_t vcsw = 0;
    std::uint64_t ivcsw = 0;
};

inline Resources sample_resources() {
    Resources r;
    r.rss_peak = peak_rss_bytes();
    r.rss_steady = current_rss_bytes();
    r.cpu_s = cpu_seconds();
    context_switches(r.vcsw, r.ivcsw);
    return r;
}

inline std::string resource_header() {
    return "role,errors,err_detail,rss_peak_mb,rss_steady_mb,cpu_s,cpu_us_per_msg,"
           "usd_per_million_msgs,vol_ctx_sw,invol_ctx_sw,usd_rate";
}

inline std::string resource_row(const std::string& role, std::uint64_t errors,
                                const std::string& err_detail, const Resources& r,
                                std::uint64_t messages) {
    char buf[512];
    const double per_msg_us = messages ? r.cpu_s * 1e6 / static_cast<double>(messages) : 0.0;
    const double usd_per_m = messages
                                 ? r.cpu_s * kUsdPerCpuSecond * 1e6 / static_cast<double>(messages)
                                 : 0.0;
    std::snprintf(buf, sizeof buf, "%s,%llu,%s,%.1f,%.1f,%.3f,%.3f,%.6f,%llu,%llu,%s", role.c_str(),
                  static_cast<unsigned long long>(errors), err_detail.c_str(),
                  static_cast<double>(r.rss_peak) / 1048576.0,
                  static_cast<double>(r.rss_steady) / 1048576.0, r.cpu_s, per_msg_us, usd_per_m,
                  static_cast<unsigned long long>(r.vcsw),
                  static_cast<unsigned long long>(r.ivcsw), kRateLabel);
    return buf;
}

// The latency CSV path with ".resources.csv" in place of its extension, so the resource
// row lands beside the run it belongs to without changing the documented latency schema.
inline std::string resource_path(const std::string& out) {
    if (out.empty()) {
        return {};
    }
    const auto dot = out.find_last_of('.');
    return (dot == std::string::npos ? out : out.substr(0, dot)) + ".resources.csv";
}

} // namespace bench
