// Guest-thread CPU affinity (cpu_affinity.hpp).
#include "lgp/platform/cpu_affinity.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>

#if defined(__linux__)
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace lgp::platform {

std::vector<int> parse_cpu_list(const std::string& text) {
    std::set<int> out;
    size_t i = 0;
    auto number = [&](int& v) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9')
            return false;
        v = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            v = v * 10 + (text[i++] - '0');
            if (v > 4095)
                return false;
        }
        return true;
    };
    while (i < text.size()) {
        int a = 0, b = 0;
        if (!number(a))
            return {};
        b = a;
        if (i < text.size() && text[i] == '-') {
            ++i;
            if (!number(b) || b < a)
                return {};
        }
        for (int c = a; c <= b; ++c) out.insert(c);
        if (i < text.size()) {
            if (text[i] != ',')
                return {};
            ++i;
        }
    }
    return {out.begin(), out.end()};
}

std::string format_cpu_list(const std::vector<int>& cpus) {
    std::string out;
    for (size_t i = 0; i < cpus.size();) {
        size_t j = i;
        while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
        if (!out.empty())
            out += ',';
        out += std::to_string(cpus[i]);
        if (j > i)
            out += '-' + std::to_string(cpus[j]);
        i = j + 1;
    }
    return out.empty() ? "-" : out;
}

std::vector<int> fastest_cpus(const std::vector<uint64_t>& max_freq_khz) {
    uint64_t lo = 0, hi = 0;
    for (uint64_t f : max_freq_khz) {
        if (f == 0)
            continue;
        lo = lo == 0 ? f : std::min(lo, f);
        hi = std::max(hi, f);
    }
    std::vector<int> out;
    if (lo == hi)
        return out;
    for (size_t c = 0; c < max_freq_khz.size(); ++c)
        if (max_freq_khz[c] > lo)
            out.push_back(static_cast<int>(c));
    return out;
}

namespace {

std::string read_line(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

uint64_t read_number(const std::string& path) {
    const std::string s = read_line(path);
    return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 10);
}

}  // namespace

std::vector<uint64_t> cpu_speeds(const std::string& root, int cpus) {
    std::vector<uint64_t> out(static_cast<size_t>(std::max(cpus, 0)), 0);
    // cpu_capacity (the scheduler's own ranking) when every CPU has one.
    bool all = !out.empty();
    for (int c = 0; c < cpus && all; ++c)
        all = (out[c] = read_number(root + "/cpu" + std::to_string(c) + "/cpu_capacity")) != 0;
    if (all)
        return out;
    // Otherwise the maximum frequency per cpufreq policy, given to every CPU
    // of the policy (related_cpus also lists offline CPUs, whose own cpufreq
    // directory may be missing).
    std::fill(out.begin(), out.end(), 0);
    for (int p = 0; p < cpus; ++p) {
        const std::string dir = root + "/cpufreq/policy" + std::to_string(p);
        const uint64_t f = read_number(dir + "/cpuinfo_max_freq");
        if (f == 0)
            continue;
        std::string related = read_line(dir + "/related_cpus");
        for (char& ch : related)
            if (ch == ' ')
                ch = ',';
        while (!related.empty() && related.back() == ',') related.pop_back();
        for (int c : parse_cpu_list(related.empty() ? std::to_string(p) : related))
            if (c < cpus)
                out[c] = f;
    }
    for (int c = 0; c < cpus; ++c)
        if (out[c] == 0)
            out[c] = read_number(root + "/cpu" + std::to_string(c) + "/cpufreq/cpuinfo_max_freq");
    return out;
}

#if defined(__linux__)
namespace {


std::vector<int> allowed_cpus() {
    std::vector<int> out;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        return out;
    for (int c = 0; c < CPU_SETSIZE; ++c)
        if (CPU_ISSET(c, &set))
            out.push_back(c);
    return out;
}

struct Policy {
    bool apply = false;
    cpu_set_t set;
};

Policy decide() {
    Policy p;
    CPU_ZERO(&p.set);
    const char* env = std::getenv("LGP_CPU_AFFINITY");
#if defined(__ANDROID__)
    // Measured on the OnePlus 7 Pro (docs/knowledge/android-ndk-build.md):
    // without pinning EAS parks the guest threads on the little cores.
    const std::string mode = env != nullptr && *env != '\0' ? env : "big";
#else
    const std::string mode = env != nullptr ? env : "";
#endif
    const std::vector<int> before = allowed_cpus();
#if defined(__ANDROID__)
    const bool report = true;
#else
    const bool report = !mode.empty();
#endif
    std::string cpuset = read_line("/proc/self/cpuset");
    if (cpuset.empty())
        cpuset = "?";
    if (mode.empty() || mode == "all") {
        if (report)
            std::fprintf(stderr, "cpu affinity: unchanged, allowed %s, cpuset %s\n", format_cpu_list(before).c_str(),
                         cpuset.c_str());
        return p;
    }
    const long n = sysconf(_SC_NPROCESSORS_CONF);
    const std::vector<int> want =
        mode == "big" ? fastest_cpus(cpu_speeds("/sys/devices/system/cpu", static_cast<int>(std::min<long>(n, CPU_SETSIZE))))
                      : parse_cpu_list(mode);
    for (int c : want)
        if (c < CPU_SETSIZE)
            CPU_SET(c, &p.set);
    p.apply = CPU_COUNT(&p.set) > 0;
    std::fprintf(stderr, "cpu affinity: LGP_CPU_AFFINITY=%s -> %s%s, allowed before %s, cpuset %s\n", mode.c_str(),
                 format_cpu_list(want).c_str(), p.apply ? "" : " (ignored)", format_cpu_list(before).c_str(),
                 cpuset.c_str());
    return p;
}

const Policy& policy() {
    static std::once_flag once;
    static Policy p;
    std::call_once(once, [] { p = decide(); });
    return p;
}

std::mutex g_threads_mutex;
std::set<pid_t> g_threads;  // guest threads with the policy (reapply_thread_cpu_policy)

// Pins `tid` (0: the calling thread) to the policy's CPUs within those it may
// use now, so a narrower mask (taskset, a cpuset) is never widened. Nothing
// changes when the two do not overlap.
void pin(pid_t tid) {
    static std::atomic<bool> warned{false};
    const Policy& p = policy();
    cpu_set_t now, set;
    CPU_ZERO(&now);
    if (sched_getaffinity(tid, sizeof(now), &now) != 0)
        return;
    CPU_AND(&set, &now, &p.set);
    if (CPU_COUNT(&set) == 0) {
        if (!warned.exchange(true))
            std::fprintf(stderr, "cpu affinity: none of the chosen CPUs allowed (allowed %s), unchanged\n",
                         format_cpu_list(allowed_cpus()).c_str());
        return;
    }
    if (sched_setaffinity(tid, sizeof(set), &set) != 0 && !warned.exchange(true))
        std::fprintf(stderr, "cpu affinity: sched_setaffinity failed, allowed %s\n",
                     format_cpu_list(allowed_cpus()).c_str());
}

}  // namespace

void apply_thread_cpu_policy() {
    if (!policy().apply)
        return;
    {
        std::lock_guard<std::mutex> lock(g_threads_mutex);
        g_threads.insert(static_cast<pid_t>(syscall(SYS_gettid)));
    }
    pin(0);
}

void forget_thread_cpu_policy() {
    if (!policy().apply)
        return;
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    g_threads.erase(static_cast<pid_t>(syscall(SYS_gettid)));
}

void reapply_thread_cpu_policy() {
    if (!policy().apply)
        return;
    // Moving a task between cpusets (e.g. background -> top-app) resets its
    // CPU mask to the new cpuset on older kernels.
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    for (pid_t tid : g_threads) pin(tid);
}
#else
void apply_thread_cpu_policy() {}
void forget_thread_cpu_policy() {}
void reapply_thread_cpu_policy() {}
#endif

}  // namespace lgp::platform
