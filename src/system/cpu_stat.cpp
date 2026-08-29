#include "system/cpu_stat.h"

#include "util/sys_utils.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#if defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

namespace noctalia::system::cpu_stat {

  namespace {

    // The label is everything before the first space: "cpu" for the aggregate row, "cpuN" for a
    // core. Returns an empty view for a row with no space at all.
    [[nodiscard]] std::string_view labelOf(const std::string& line) {
      const auto space = line.find(' ');
      return space == std::string::npos ? std::string_view{} : std::string_view{line}.substr(0, space);
    }

    [[nodiscard]] bool isCoreLabel(std::string_view label) { return SysUtils::isCpuN(label); }

  } // namespace

  std::optional<Totals> parseLine(const std::string& line, std::string_view expectedLabel) {
    std::istringstream iss{line};
    std::string cpuLabel;
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    std::uint64_t steal = 0;

    // user/nice/system/idle are mandatory, so a row with a plausible label but unreadable counters
    // is rejected rather than read as all-zero. The trailing fields are optional: a kernel
    // predating one leaves it zero-initialised rather than poisoning the sum.
    if (!(iss >> cpuLabel >> user >> nice >> system >> idle)) {
      return std::nullopt;
    }
    if (cpuLabel != expectedLabel) {
      return std::nullopt;
    }
    iss >> iowait >> irq >> softirq >> steal;

    Totals totals{};
    totals.idle = idle + iowait;
    totals.total = user + nice + system + idle + iowait + irq + softirq + steal;
    return totals;
  }

  std::optional<double> usageBetween(const Totals& prev, const Totals& current) {
    if (current.total <= prev.total) {
      return std::nullopt;
    }
    const std::uint64_t totalDelta = current.total - prev.total;
    const std::uint64_t idleDelta = current.idle >= prev.idle ? current.idle - prev.idle : 0;
    const double busy = 1.0 - (static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
    return std::clamp(100.0 * busy, 0.0, 100.0);
  }

#if defined(__FreeBSD__)
  namespace {
    // FreeBSD CPUSTATES layout: CP_USER, CP_NICE, CP_SYS, CP_INTR, CP_IDLE.
    constexpr int kCpuStates = 5;
    constexpr int kIdleSlot = 4;

    [[nodiscard]] std::optional<Totals> totalsFromTimes(const long* times) {
      Totals totals{};
      for (int i = 0; i < kCpuStates; ++i) {
        if (times[i] < 0) {
          return std::nullopt;
        }
        totals.total += static_cast<std::uint64_t>(times[i]);
      }
      if (times[kIdleSlot] >= 0) {
        totals.idle = static_cast<std::uint64_t>(times[kIdleSlot]);
      }
      return totals;
    }
  } // namespace
#endif

  std::optional<Totals> readTotals(const std::filesystem::path& statPath) {
#if defined(__linux__)
    std::ifstream file{statPath};
    if (!file.is_open()) {
      return std::nullopt;
    }

    std::string line;
    if (!std::getline(file, line)) {
      return std::nullopt;
    }

    return parseLine(line, "cpu");
#else
    (void)statPath;
    long times[kCpuStates] = {};
    std::size_t length = sizeof(times);
    if (::sysctlbyname("kern.cp_time", times, &length, nullptr, 0) != 0 || length < sizeof(times)) {
      return std::nullopt;
    }
    return totalsFromTimes(times);
#endif
  }

  std::optional<std::vector<Totals>> readCoreTotals(const std::filesystem::path& statPath) {
#if defined(__linux__)
    std::ifstream file{statPath};
    if (!file.is_open()) {
      return std::nullopt;
    }

    std::vector<Totals> cores;
    std::string line;
    // The cpu rows lead the file, so stop at the first row that is not one rather than scanning
    // the whole of /proc/stat.
    while (std::getline(file, line)) {
      const std::string_view label = labelOf(line);
      if (label == "cpu") {
        continue; // the aggregate row
      }
      if (!isCoreLabel(label)) {
        break;
      }
      // The expected label comes from the row itself: offline cores are omitted from /proc/stat,
      // so predicting "cpu" + cores.size() would desync permanently at the first gap.
      const auto totals = parseLine(line, label);
      if (!totals.has_value()) {
        return std::nullopt;
      }
      cores.push_back(*totals);
    }

    if (cores.empty()) {
      return std::nullopt;
    }
    return cores;
#else
    (void)statPath;
    int coreCount = 0;
    std::size_t length = sizeof(coreCount);
    if (::sysctlbyname("kern.smp.cpus", &coreCount, &length, nullptr, 0) != 0 || coreCount <= 0) {
      return std::nullopt;
    }

    std::vector<long> times(static_cast<std::size_t>(coreCount) * kCpuStates, -1L);
    length = times.size() * sizeof(long);
    if (::sysctlbyname("kern.cp_times", times.data(), &length, nullptr, 0) != 0) {
      return std::nullopt;
    }

    const auto reported = length / sizeof(long);
    const auto usableCores = static_cast<std::size_t>(reported / kCpuStates);
    if (usableCores == 0) {
      return std::nullopt;
    }

    std::vector<Totals> cores;
    cores.reserve(usableCores);
    for (std::size_t core = 0; core < usableCores; ++core) {
      auto totals = totalsFromTimes(times.data() + core * kCpuStates);
      if (!totals.has_value()) {
        return std::nullopt;
      }
      cores.push_back(*totals);
    }
    return cores;
#endif
  }

} // namespace noctalia::system::cpu_stat
