#include "core/inotify/inotify.h"

#include "core/log.h"

#include <optional>
#if defined(__linux__)
#include <sys/inotify.h>
#endif
#include <unistd.h>

namespace {
  constexpr Logger kLog("inotify");
}

#if defined(__linux__)

Inotify::Inotify() {
  m_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (m_inotifyFd < 0)
    kLog.warn("inotify_init1 failed");
}

Inotify::~Inotify() {
  if (m_inotifyFd < 0)
    return;
  for (auto& wd : m_watchDescriptors)
    inotify_rm_watch(m_inotifyFd, wd);
  ::close(m_inotifyFd);
}

std::optional<int> Inotify::watch(const std::filesystem::path& path, Inotify::WatchMask mask) noexcept {
  if (m_inotifyFd < 0)
    return std::nullopt;

  auto dir = path.string();

  int wd = inotify_add_watch(m_inotifyFd, dir.c_str(), mask);
  if (wd < 0) {
    kLog.warn("failed to watch directory '{}'", dir);
    return std::nullopt;
  }
  m_watchDescriptors.insert(wd);

  return wd;
}

void Inotify::unwatch(int wd) {
  if (m_inotifyFd >= 0)
    inotify_rm_watch(m_inotifyFd, wd);
  m_watchDescriptors.erase(wd);
}

void Inotify::drain(std::optional<Callback> global_callback) noexcept {
  if (m_inotifyFd < 0)
    return;

  alignas(inotify_event) char buf[4096];

  while (true) {
    const ssize_t n = ::read(m_inotifyFd, buf, sizeof(buf));
    if (n <= 0)
      break;

    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(n)) {
      const auto* event = reinterpret_cast<inotify_event*>(buf + offset);

      if ((event->mask & IN_IGNORED) != 0) {
        // watch was removed somehow => remove watch id
        m_watchDescriptors.erase(event->wd);
      } else if (
          global_callback.has_value() && ((event->mask & IN_Q_OVERFLOW) != 0 || m_watchDescriptors.contains(event->wd))
      ) {
        (*global_callback)(event);
      }

      offset += sizeof(inotify_event) + event->len;
    }
  }
}

#elif defined(__FreeBSD__)

#include <fcntl.h>
#include <sys/event.h>
#include <sys/stat.h>

#include <cstdio>
#include <dirent.h>

namespace {

  constexpr unsigned int kVnodeFlags = NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME;

} // namespace

Inotify::Inotify() {
  m_kqueueFd = ::kqueue();
  if (m_kqueueFd < 0)
    kLog.warn("kqueue() failed");
}

Inotify::~Inotify() {
  for (auto& [ /* wd */ _, watch] : m_watches)
    clearFileNotes(watch);
  if (m_kqueueFd >= 0)
    ::close(m_kqueueFd);
}

bool Inotify::registerFileNote(DirWatch& watch, const std::string& name) {
  const int fd = ::openat(watch.dirFd, name.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
    return false;
  struct kevent change{};
  EV_SET(&change, static_cast<uintptr_t>(fd), EVFILT_VNODE, EV_ADD | EV_CLEAR, kVnodeFlags, 0, nullptr);
  if (::kevent(m_kqueueFd, &change, 1, nullptr, 0, nullptr) < 0) {
    ::close(fd);
    return false;
  }
  watch.fileFds.emplace(name, fd);
  return true;
}

void Inotify::clearFileNotes(DirWatch& watch) {
  for (auto& [ /* name */ _, fd] : watch.fileFds) {
    struct kevent change{};
    EV_SET(&change, static_cast<uintptr_t>(fd), EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
    ::kevent(m_kqueueFd, &change, 1, nullptr, 0, nullptr);
    ::close(fd);
  }
  watch.fileFds.clear();
}

std::optional<int> Inotify::watch(const std::filesystem::path& path, Inotify::WatchMask /*mask*/) noexcept {
  if (m_kqueueFd < 0)
    return std::nullopt;

  const auto dirStr = path.string();
  const int dirFd = ::open(dirStr.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dirFd < 0) {
    kLog.warn("failed to open directory '{}' for watching", dirStr);
    return std::nullopt;
  }

  // Idempotent per inode: re-watching an already-watched directory replaces the
  // vnode note (EV_ADD is a no-op for an existing ident/filter pair).
  struct kevent change{};
  EV_SET(&change, static_cast<uintptr_t>(dirFd), EVFILT_VNODE, EV_ADD | EV_CLEAR, kVnodeFlags, 0, nullptr);
  if (::kevent(m_kqueueFd, &change, 1, nullptr, 0, nullptr) < 0) {
    kLog.warn("failed to register vnode watch for '{}'", dirStr);
    ::close(dirFd);
    return std::nullopt;
  }

  DirWatch watch;
  watch.dirFd = dirFd;
  watch.path = path;
  watch.snapshot = scanDirectory(path);

  const int wd = dirFd; // the fd doubles as the public watch descriptor
  auto [it, inserted] = m_watches.insert_or_assign(wd, std::move(watch));
  (void)inserted;

  // Register per-file vnode notes so in-place writes to tracked files are seen
  // (directory-level notes only fire on create/delete/rename).
  auto& stored = it->second;
  for (const auto& [name, state] : stored.snapshot) {
    if (!state.isDir && registerFileNote(stored, name)) {
      m_fileFdToWatch.emplace(stored.fileFds.at(name), wd);
    }
  }

  kLog.debug("watching directory '{}' (wd {})", dirStr, wd);
  return wd;
}

void Inotify::unwatch(int wd) {
  auto it = m_watches.find(wd);
  if (it == m_watches.end())
    return;

  clearFileNotes(it->second);

  struct kevent change{};
  EV_SET(&change, static_cast<uintptr_t>(it->second.dirFd), EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
  ::kevent(m_kqueueFd, &change, 1, nullptr, 0, nullptr); // ignore errors (already gone)

  ::close(it->second.dirFd);
  m_watches.erase(it);
}

std::map<std::string, Inotify::EntryState> Inotify::scanDirectory(const std::filesystem::path& path) {
  std::map<std::string, EntryState> snapshot;

  DIR* dir = ::opendir(path.c_str());
  if (dir == nullptr)
    return snapshot;

  while (const auto* entry = ::readdir(dir)) {
    const std::string_view name{entry->d_name};
    if (name == "." || name == "..")
      continue;
    struct stat st{};
    if (::fstatat(::dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
      continue;
    EntryState state;
    state.inode = static_cast<std::uint64_t>(st.st_ino);
    state.mtimeNs = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000LL + st.st_mtim.tv_nsec;
    state.size = static_cast<std::int64_t>(st.st_size);
    state.isDir = S_ISDIR(st.st_mode);
    snapshot.emplace(std::string{name}, state);
  }
  ::closedir(dir);
  return snapshot;
}

bool Inotify::entryChanged(const EntryState& previous, const EntryState& current) noexcept {
  return previous.inode != current.inode || previous.mtimeNs != current.mtimeNs || previous.size != current.size;
}

void Inotify::emitEvent(const Callback& callback, int wd, std::uint32_t mask, const std::string& name) const {
  alignas(inotify_event) char buf[sizeof(inotify_event)];
  auto* event = reinterpret_cast<inotify_event*>(buf);
  event->wd = wd;
  event->mask = mask;
  event->cookie = 0;
  event->len = 1; // non-zero signals "named event" downstream
  std::snprintf(event->name, sizeof(event->name), "%s", name.c_str());
  callback(event);
}

void Inotify::drain(std::optional<Callback> global_callback) noexcept {
  if (m_kqueueFd < 0)
    return;

  struct kevent events[32];

  while (true) {
    timespec timeout{}; // non-blocking drain
    const int n = ::kevent(m_kqueueFd, nullptr, 0, events, static_cast<int>(std::size(events)), &timeout);
    if (n <= 0)
      break;

    // Collect the affected watches; several knotes may map to one directory.
    std::set<int> affectedWds;
    for (int i = 0; i < n; ++i) {
      const auto& kev = events[i];
      if (kev.filter != EVFILT_VNODE)
        continue;

      const int ident = static_cast<int>(static_cast<intptr_t>(kev.ident));
      if ((kev.fflags & (NOTE_DELETE | NOTE_RENAME)) != 0) {
        if (m_watches.contains(ident)) { // the watched directory itself vanished
          kLog.debug("watched directory vanished (wd {})", ident);
          unwatch(ident);
        }
        continue;
      }
      if ((kev.fflags & (NOTE_WRITE | NOTE_EXTEND)) == 0)
        continue;

      if (m_watches.contains(ident)) {
        affectedWds.insert(ident);
      } else if (const auto owner = m_fileFdToWatch.find(ident); owner != m_fileFdToWatch.end()) {
        affectedWds.insert(owner->second);
      }
    }

    for (const int wd : affectedWds) {
      auto it = m_watches.find(wd);
      if (it == m_watches.end())
        continue;
      auto& watch = it->second;

      const auto current = scanDirectory(watch.path);
      if (current.empty() && !std::filesystem::exists(watch.path)) {
        // Directory disappeared between the note and the rescan.
        unwatch(wd);
        continue;
      }
      auto& snapshot = watch.snapshot;

      for (const auto& [name, state] : current) {
        std::uint32_t mask = 0;
        const auto prev = snapshot.find(name);
        if (prev == snapshot.end()) {
          mask = IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE;
        } else if (entryChanged(prev->second, state)) {
          mask = IN_MODIFY | IN_CLOSE_WRITE;
        }
        if (mask != 0 && global_callback.has_value()) {
          emitEvent(*global_callback, wd, mask, name);
        }
        // Keep per-file notes fresh: register for new files.
        if (!state.isDir && !watch.fileFds.contains(name)) {
          if (registerFileNote(watch, name)) {
            m_fileFdToWatch.emplace(watch.fileFds.at(name), wd);
          }
        }
      }

      for (const auto& [name, /* state */ _] : snapshot) {
        if (!current.contains(name)) {
          if (global_callback.has_value()) {
            emitEvent(*global_callback, wd, IN_DELETE, name);
          }
          // Drop the stale per-file note.
          if (const auto fdIt = watch.fileFds.find(name); fdIt != watch.fileFds.end()) {
            struct kevent change{};
            EV_SET(&change, static_cast<uintptr_t>(fdIt->second), EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
            ::kevent(m_kqueueFd, &change, 1, nullptr, 0, nullptr);
            ::close(fdIt->second);
            m_fileFdToWatch.erase(fdIt->second);
            watch.fileFds.erase(fdIt);
          }
        }
      }

      snapshot = std::move(current);
    }
  }
}

#else
#error "No file-monitor backend for this platform"
#endif
