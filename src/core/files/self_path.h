#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

// Resolves the path of the currently running executable in a portable way:
// Linux reads /proc/self/exe, FreeBSD queries KERN_PROC_PATHNAME via sysctl.
// Returns std::nullopt when the platform provides no mechanism or the query fails.
namespace SelfPath {

  [[nodiscard]] std::optional<std::filesystem::path> selfExePath();

} // namespace SelfPath
