// Which host CPUs guest threads may run on (P7.3). On phones with big.LITTLE
// clusters the scheduler can leave a busy process on the slow cores; pinning
// the guest threads to the fast clusters is the default on Android (measured
// gain) and opt-in elsewhere (LGP_CPU_AFFINITY). Only Linux and Android apply
// it; elsewhere the policy is a no-op.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lgp::platform {

// "4-7", "0,2,4-5"; empty result for anything malformed.
std::vector<int> parse_cpu_list(const std::string& text);
std::string format_cpu_list(const std::vector<int>& cpus);

// CPUs outside the slowest cluster, by a per-CPU speed (index = CPU,
// 0 = unknown). Empty when all known CPUs share one speed.
std::vector<int> fastest_cpus(const std::vector<uint64_t>& max_freq_khz);

// Per-CPU speed below a sysfs CPU directory (/sys/devices/system/cpu):
// cpu<N>/cpu_capacity if every CPU has one (also tells clusters with the same
// top frequency apart), else cpufreq/policy<N>/cpuinfo_max_freq for all its
// related_cpus (offline CPUs included), else cpu<N>/cpufreq/cpuinfo_max_freq.
std::vector<uint64_t> cpu_speeds(const std::string& root, int cpus);

// Applies LGP_CPU_AFFINITY to the calling thread: "all" (and, except on
// Android, unset or "") leaves it alone, "big" (the Android default) picks
// fastest_cpus() of this device, anything else is a CPU list. The first call prints one line with the result (and on Android the
// process's cpuset and allowed CPUs, also without the variable). The chosen
// CPUs are intersected with those the thread may use, so a narrower mask is
// never widened.
void apply_thread_cpu_policy();

// The calling thread leaves the set reapply_thread_cpu_policy() pins (call
// before a thread that called apply_thread_cpu_policy() ends).
void forget_thread_cpu_policy();

// Pins every thread that applied the policy again. For the app's return to
// the foreground: moving between cpusets resets the CPU masks on older
// Android kernels (Verdacht, 4.14 on the OnePlus 7 Pro).
void reapply_thread_cpu_policy();

}  // namespace lgp::platform
