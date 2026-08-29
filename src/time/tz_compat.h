#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

// Time-zone conversion helpers built on the C timezone API (tzset/localtime_r/
// mktime). They exist because libc++ still lacks the std::chrono time-zone
// database (current_zone/locate_zone), which Noctalia's clock and calendar
// features rely on; libstdc++ has it, but sharing one implementation keeps
// behaviour identical across platforms and standard libraries.
//
// Thread-safety: switching the process-wide TZ for named-zone queries is
// serialized inside this module, but concurrent localtime_r() callers outside
// of it could theoretically observe a transient TZ switch. In practice these
// queries run at UI update rates and the window is microseconds wide.
namespace noctalia::tz {

  using SysSeconds = std::chrono::sys_seconds;
  using LocalSeconds = std::chrono::local_seconds;

  struct ZoneReading {
    LocalSeconds local{};
    std::chrono::seconds offset{}; ///< UTC offset in effect at that instant.
    std::chrono::minutes save{};   ///< Non-zero while daylight saving applies.
    std::string abbrev;            ///< Zone abbreviation, e.g. "CET".
    bool ok = false;
  };

  /// True when `name` resolves to an installed zoneinfo database entry.
  [[nodiscard]] bool zoneExists(std::string_view name);

  /// Convert a UTC instant to wall-clock time in `name` (empty = system zone).
  [[nodiscard]] ZoneReading toLocal(std::string_view zoneName, SysSeconds tp);

  /// Interpret a wall-clock time in `name` (empty = system zone) as UTC.
  /// Returns nullopt for dates the conversion API cannot represent.
  [[nodiscard]] std::optional<SysSeconds>
  toSys(std::string_view zoneName, int year, unsigned month, unsigned day, int hour, int minute, int second);

} // namespace noctalia::tz
