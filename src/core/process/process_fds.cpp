#include "core/process/process_fds.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <format>
#include <string>
#include <string_view>
#include <sys/resource.h>
#if defined(__FreeBSD__)
#include <sys/user.h>
#include <libutil.h>
#endif
#include <unistd.h>
#include <unordered_map>
#include <vector>


namespace {

  constexpr Logger kLog("fdlimit");

  [[nodiscard]] bool isFdName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
      return false;
    }
    for (const char* p = name; *p != '\0'; ++p) {
      if (*p < '0' || *p > '9') {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::string rlimitValue(rlim_t value) {
    if (value == RLIM_INFINITY) {
      return "infinity";
    }
    return std::to_string(static_cast<unsigned long long>(value));
  }

  [[nodiscard]] std::string rlimitSummary() {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
      return "rlimit_nofile=unavailable";
    }
    return std::format("rlimit_nofile={}/{}", rlimitValue(limit.rlim_cur), rlimitValue(limit.rlim_max));
  }

  [[nodiscard]] bool startsWith(std::string_view value, std::string_view prefix) { return value.starts_with(prefix); }

  [[nodiscard]] std::string bucketTarget(std::string target) {
    if (startsWith(target, "socket:")) {
      return "socket";
    }
    if (startsWith(target, "pipe:")) {
      return "pipe";
    }
    if (startsWith(target, "memfd:") || startsWith(target, "/memfd:")) {
      return "memfd";
    }
    if (startsWith(target, "anon_inode:")) {
      return target;
    }
    if (target.size() > 120) {
      target.resize(117);
      target += "...";
    }
    return target;
  }

  [[nodiscard]] std::vector<std::pair<std::string, std::size_t>>
  sortedTargets(const std::unordered_map<std::string, std::size_t>& targetCounts) {
    std::vector<std::pair<std::string, std::size_t>> targets;
    targets.reserve(targetCounts.size());
    for (auto& [target, targetCount] : targetCounts) {
      targets.emplace_back(target, targetCount);
    }
    std::ranges::sort(targets, [](const auto& lhs, const auto& rhs) {
      if (lhs.second != rhs.second) {
        return lhs.second > rhs.second;
      }
      return lhs.first < rhs.first;
    });
    return targets;
  }

  [[nodiscard]] std::string readFdTarget(const char* fdName) {
    const std::string path = std::string("/proc/self/fd/") + fdName;
    std::vector<char> buffer(512);
    while (true) {
      const ssize_t n = readlink(path.c_str(), buffer.data(), buffer.size() - 1);
      if (n < 0) {
        return std::format("readlink failed: {}", std::strerror(errno));
      }
      if (static_cast<std::size_t>(n) < buffer.size() - 1) {
        buffer[static_cast<std::size_t>(n)] = '\0';
        return std::string(buffer.data());
      }
      buffer.resize(buffer.size() * 2U);
      if (buffer.size() > 8192U) {
        return "<fd target too long>";
      }
    }
  }

} // namespace

void ProcessFds::raiseOpenFileLimit() {
  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    kLog.error("RLIMIT_NOFILE getrlimit failed: {}", std::strerror(errno));
    return;
  }

  const rlim_t previous = limit.rlim_cur;
  if (limit.rlim_cur >= limit.rlim_max) {
    kLog.info("RLIMIT_NOFILE already at hard limit ({})", rlimitValue(previous));
    return;
  }

  limit.rlim_cur = limit.rlim_max;
  if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
    kLog.error(
        "RLIMIT_NOFILE setrlimit to {} failed: {} (soft limit stays {})", rlimitValue(limit.rlim_max),
        std::strerror(errno), rlimitValue(previous)
    );
    return;
  }

  kLog.info("RLIMIT_NOFILE soft limit raised {} -> {}", rlimitValue(previous), rlimitValue(limit.rlim_max));
}

std::string ProcessFds::describeOpenFileDescriptors(std::size_t maxTargets) {
  const std::string limit = rlimitSummary();

#if defined(__FreeBSD__)
  // /proc/self/fd requires mounted procfs on FreeBSD; the native source is
  // KERN_PROC_FILEDESC via kinfo_getfile(3).
  int count = 0;
  kinfo_file* entries = kinfo_getfile(::getpid(), &count);
  if (entries == nullptr) {
    return std::format("open_fds=unavailable (kinfo_getfile failed), {}", limit);
  }

  auto bucketForType = [](int type) -> std::string {
    switch (type) {
    case KF_TYPE_VNODE:
      return "vnode";
    case KF_TYPE_SOCKET:
      return "socket";
    case KF_TYPE_PIPE:
      return "pipe";
    case KF_TYPE_FIFO:
      return "fifo";
    case KF_TYPE_KQUEUE:
      return "kqueue";
    case KF_TYPE_MQUEUE:
      return "mqueue";
    case KF_TYPE_SHM:
      return "shm";
    case KF_TYPE_SEM:
      return "sem";
    case KF_TYPE_PTS:
      return "pts";
    case KF_TYPE_DEV:
      return "dev";
    case KF_TYPE_EVENTFD:
      return "eventfd";
    default:
      return "other";
    }
  };

  std::unordered_map<std::string, std::size_t> targetCounts;
  for (int i = 0; i < count; ++i) {
    const kinfo_file& entry = entries[i];
    if (entry.kf_fd < 0) {
      continue; // closed slot
    }
    ++count;
    std::string target;
    if (entry.kf_type == KF_TYPE_VNODE && entry.kf_path[0] != '\0') {
      target = bucketTarget(std::string{entry.kf_path});
    } else if (entry.kf_type == KF_TYPE_SHM && entry.kf_path[0] != '\0') {
      target = bucketTarget("shm:" + std::string{entry.kf_path});
    } else {
      target = bucketForType(entry.kf_type);
    }
    ++targetCounts[std::move(target)];
  }
  ::free(entries);

  const auto targets = sortedTargets(targetCounts);
#else
  DIR* dir = opendir("/proc/self/fd");
  if (dir == nullptr) {
    return std::format("open_fds=unavailable (opendir /proc/self/fd failed: {}), {}", std::strerror(errno), limit);
  }

  std::size_t count = 0;
  std::unordered_map<std::string, std::size_t> targetCounts;
  const int directoryFd = dirfd(dir);
  while (dirent* entry = readdir(dir)) {
    if (!isFdName(entry->d_name)) {
      continue;
    }
    char* end = nullptr;
    const long fdNumber = std::strtol(entry->d_name, &end, 10);
    if (end != nullptr && *end == '\0' && fdNumber == directoryFd) {
      continue;
    }
    ++count;
    ++targetCounts[bucketTarget(readFdTarget(entry->d_name))];
  }
  closedir(dir);

  const auto targets = sortedTargets(targetCounts);
#endif

  std::string out = std::format("open_fds={}, {}", count, limit);
  if (!targets.empty() && maxTargets > 0) {
    out += ", top_fd_targets=[";
    const std::size_t n = std::min(maxTargets, targets.size());
    for (std::size_t i = 0; i < n; ++i) {
      if (i != 0) {
        out += ", ";
      }
      out += std::format("{}={}", targets[i].first, targets[i].second);
    }
    out += "]";
  }
  return out;
}
