#include "core/files/self_path.h"

#if defined(__linux__)
#include <unistd.h>
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

namespace SelfPath {

#if defined(__linux__)
  std::optional<std::filesystem::path> selfExePath() {
    std::array<char, 4096> buffer{};
    const ssize_t count = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size() - 1) {
      return std::nullopt;
    }
    buffer[static_cast<std::size_t>(count)] = '\0';
    return std::filesystem::path(buffer.data());
  }
#elif defined(__FreeBSD__)
  std::optional<std::filesystem::path> selfExePath() {
    std::array<char, 4096> buffer{};
    std::size_t length = buffer.size() - 1;
    int mib[4]{CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    if (::sysctl(mib, 4, buffer.data(), &length, nullptr, 0) != 0 || length == 0) {
      return std::nullopt;
    }
    buffer[length] = '\0';
    // Unlike Linux, sysctl appends the terminating NUL to the reported length.
    if (length > 0 && buffer[length - 1] == '\0') {
      --length;
    }
    return std::filesystem::path(std::string_view{buffer.data(), length});
  }
#else
  std::optional<std::filesystem::path> selfExePath() {
    return std::nullopt;
  }
#endif

} // namespace SelfPath
