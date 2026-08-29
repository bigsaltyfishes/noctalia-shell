#include "time/tz_compat.h"

#include <ctime>
#include <filesystem>
#include <mutex>
#include <string_view>

#include <cstdlib>
#include <string>
#include <type_traits>

namespace noctalia::tz {
  namespace {

    std::mutex& tzMutex() {
      static std::mutex mutex;
      return mutex;
    }

    void restoreTz(const char* previous, const std::string& previousValue) {
      if (previous == nullptr) {
        ::unsetenv("TZ");
      } else {
        ::setenv("TZ", previousValue.c_str(), 1);
      }
      ::tzset();
    }

    /// Runs `fn` with TZ set to the given zone; restores the previous value.
    /// An empty zone name leaves the process timezone untouched.
    template <typename Fn>
    auto withTz(std::string_view zoneName, Fn&& fn) -> decltype(fn()) {
      if (zoneName.empty()) {
        return fn();
      }
      std::scoped_lock lock(tzMutex());
      const char* previous = ::getenv("TZ");
      const std::string previousValue = previous != nullptr ? previous : "";

      const std::string name(zoneName);
      ::setenv("TZ", name.c_str(), 1);
      ::tzset();

      if constexpr (std::is_void_v<decltype(fn())>) {
        fn();
        restoreTz(previous, previousValue);
      } else {
        auto result = fn();
        restoreTz(previous, previousValue);
        return result;
      }
    }

    LocalSeconds localFromBrokenDown(const std::tm& tm) {
      using namespace std::chrono;
      const year_month_day ymd{year{tm.tm_year + 1900} / month{static_cast<unsigned>(tm.tm_mon + 1)}
                               / day{static_cast<unsigned>(tm.tm_mday)}};
      const local_days ld{ymd};
      return time_point_cast<seconds>(ld) + hours{tm.tm_hour} + minutes{tm.tm_min} + seconds{tm.tm_sec};
    }

    bool safeZoneName(std::string_view name) {
      // Reject anything that could escape the zoneinfo directory or is not a
      // plain database entry (POSIX TZ strings are intentionally excluded).
      if (name.empty() || name.size() > 255 || name.front() == '/') {
        return false;
      }
      std::size_t start = 0;
      while (start <= name.size()) {
        const auto slash = name.find('/', start);
        const auto component =
            name.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (component.empty() || component == "." || component == "..") {
          return false;
        }
        if (slash == std::string_view::npos) {
          break;
        }
        start = slash + 1;
      }
      return true;
    }

  } // namespace

  bool zoneExists(std::string_view name) {
    if (!safeZoneName(name)) {
      return false;
    }
    namespace fs = std::filesystem;
    for (const char* root : {"/usr/share/zoneinfo", "/usr/lib/zoneinfo"}) {
      std::error_code ec;
      const fs::path candidate = fs::path{root} / std::string_view{name};
      if (fs::exists(candidate, ec) && !ec) {
        return true;
      }
    }
    return false;
  }

  ZoneReading toLocal(std::string_view zoneName, SysSeconds tp) {
    ZoneReading reading{};
    const std::time_t raw = std::chrono::system_clock::to_time_t(tp);

    std::tm tm{};
    if (zoneName.empty()) {
      if (::localtime_r(&raw, &tm) == nullptr) {
        return reading;
      }
    } else {
      if (!safeZoneName(zoneName)) {
        return reading;
      }
      const bool converted = withTz(zoneName, [&] { return ::localtime_r(&raw, &tm) != nullptr; });
      if (!converted) {
        return reading;
      }
    }

    reading.local = localFromBrokenDown(tm);
    reading.offset = std::chrono::seconds{
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        tm.tm_gmtoff
#else
        0
#endif
    };
    reading.save = tm.tm_isdst > 0 ? std::chrono::minutes{60} : std::chrono::minutes{0};
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    if (tm.tm_zone != nullptr) {
      reading.abbrev = tm.tm_zone;
    }
#endif
    if (reading.abbrev.empty()) {
      reading.abbrev = tm.tm_isdst > 0 ? ::tzname[1] : ::tzname[0];
    }
    reading.ok = true;
    return reading;
  }

  std::optional<SysSeconds>
  toSys(std::string_view zoneName, int year, unsigned month, unsigned day, int hour, int minute, int second) {
    if (!zoneName.empty() && !safeZoneName(zoneName)) {
      return std::nullopt;
    }

    return withTz(zoneName, [=]() -> std::optional<SysSeconds> {
      std::tm tm{};
      tm.tm_year = year - 1900;
      tm.tm_mon = static_cast<int>(month) - 1;
      tm.tm_mday = day;
      tm.tm_hour = hour;
      tm.tm_min = minute;
      tm.tm_sec = second;
      tm.tm_isdst = -1;
      const std::time_t resolved = ::mktime(&tm);
      if (resolved == static_cast<std::time_t>(-1)) {
        return std::nullopt;
      }
      return std::chrono::sys_seconds{std::chrono::seconds{resolved}};
    });
  }

} // namespace noctalia::tz
