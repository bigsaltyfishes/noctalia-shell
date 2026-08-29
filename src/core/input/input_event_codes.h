#pragma once

// Linux evdev button/key codes as they flow through the Wayland pointer and
// keyboard protocols. On Linux the kernel header is authoritative; everywhere
// else (e.g. FreeBSD) we pin the stable values ourselves so that no
// OS-specific header is needed.

#if defined(__linux__)

#include <linux/input-event-codes.h>

#else

// Button codes (linux/input-event-codes.h).
constexpr unsigned int BTN_MOUSE = 0x110;
constexpr unsigned int BTN_LEFT = 0x110;
constexpr unsigned int BTN_RIGHT = 0x111;
constexpr unsigned int BTN_MIDDLE = 0x112;
constexpr unsigned int BTN_SIDE = 0x113;
constexpr unsigned int BTN_EXTRA = 0x114;
constexpr unsigned int BTN_FORWARD = 0x115;
constexpr unsigned int BTN_BACK = 0x116;
constexpr unsigned int BTN_TASK = 0x117;

// Key codes actually referenced by Noctalia.
constexpr unsigned int KEY_LEFTCTRL = 29;
constexpr unsigned int KEY_A = 30;
constexpr unsigned int KEY_S = 31;
constexpr unsigned int KEY_C = 46;
constexpr unsigned int KEY_V = 47;
constexpr unsigned int KEY_LEFTSHIFT = 42;
constexpr unsigned int KEY_INSERT = 110;

#endif
