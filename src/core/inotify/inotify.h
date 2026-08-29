#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

#if defined(__linux__)

#include <sys/inotify.h>

class Inotify {
public:
  using WatchMask = std::uint32_t;
  using Callback = std::function<void(const inotify_event*)>;

  Inotify();
  ~Inotify();

  Inotify(const Inotify&) = delete;
  Inotify& operator=(const Inotify&) = delete;

  struct WatchEntry {
    int wd;
    std::filesystem::path path;
    WatchMask mask;
  };

  std::optional<int> watch(const std::filesystem::path& path, WatchMask mask) noexcept;

  [[nodiscard]] int fd() const noexcept { return m_inotifyFd; }

  void unwatch(int wd);

  void drain(std::optional<Callback> callback = std::nullopt) noexcept;

private:
  int m_inotifyFd = -1;
  std::set<int> m_watchDescriptors;
};

#elif defined(__FreeBSD__)

// ---------------------------------------------------------------------------
// kqueue-backed drop-in replacement for Linux inotify.
//
// EVFILT_VNODE cannot report which file inside a watched directory changed,
// so every directory event triggers a rescan of that directory; the diff
// against the previous snapshot is emitted as synthetic `inotify_event`
// records. Downstream code (FileWatcher, ConfigService) keeps its existing
// event-driven API unchanged.
// ---------------------------------------------------------------------------

struct inotify_event {
  int wd;
  std::uint32_t mask;
  std::uint32_t cookie;
  std::uint32_t len;
  char name[512];
};

// Subset of the Linux inotify masks used by Noctalia's watchers.
constexpr std::uint32_t IN_ACCESS = 0x00000001u;
constexpr std::uint32_t IN_MODIFY = 0x00000002u;
constexpr std::uint32_t IN_ATTRIB = 0x00000004u;
constexpr std::uint32_t IN_CLOSE_WRITE = 0x00000008u;
constexpr std::uint32_t IN_OPEN = 0x00000020u;
constexpr std::uint32_t IN_MOVED_FROM = 0x00000040u;
constexpr std::uint32_t IN_MOVED_TO = 0x00000080u;
constexpr std::uint32_t IN_CREATE = 0x00000100u;
constexpr std::uint32_t IN_DELETE = 0x00000200u;
constexpr std::uint32_t IN_DELETE_SELF = 0x00000400u;
constexpr std::uint32_t IN_MOVE_SELF = 0x00000800u;
constexpr std::uint32_t IN_Q_OVERFLOW = 0x00004000u;
constexpr std::uint32_t IN_IGNORED = 0x00008000u;

class Inotify {
public:
  using WatchMask = std::uint32_t;
  using Callback = std::function<void(const inotify_event*)>;

  Inotify();
  ~Inotify();

  Inotify(const Inotify&) = delete;
  Inotify& operator=(const Inotify&) = delete;

  // `mask` is accepted for interface parity; kqueue reports a single
  // NOTE_WRITE-class event per change and the snapshot diff decides which
  // synthetic masks to emit.
  std::optional<int> watch(const std::filesystem::path& path, WatchMask mask) noexcept;

  [[nodiscard]] int fd() const noexcept { return m_kqueueFd; }

  void unwatch(int wd);

  void drain(std::optional<Callback> callback = std::nullopt) noexcept;

private:
  struct EntryState {
    std::uint64_t inode = 0;
    std::int64_t mtimeNs = 0;
    std::int64_t size = -1;
    bool isDir = false;
  };

  struct DirWatch {
    int dirFd = -1;
    std::filesystem::path path;
    std::map<std::string, EntryState> snapshot;
    // Vnode notes on the contained files themselves: kqueue directory notes do
    // not fire on in-place file writes, so every tracked file gets its own.
    std::map<std::string, int> fileFds;
  };

  void emitEvent(const Callback& callback, int wd, std::uint32_t mask, const std::string& name) const;
  static std::map<std::string, EntryState> scanDirectory(const std::filesystem::path& path);
  static bool entryChanged(const EntryState& previous, const EntryState& current) noexcept;
  bool registerFileNote(DirWatch& watch, const std::string& name);
  void clearFileNotes(DirWatch& watch);

  int m_kqueueFd = -1;
  std::map<int, DirWatch> m_watches;     // key: watch descriptor == directory fd
  std::map<int, int> m_fileFdToWatch;    // file vnode fd -> owning watch descriptor
};

#else

#error "No file-monitor backend for this platform"

#endif
