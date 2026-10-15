#pragma once

#ifdef HAVE_WIN_KEYBOARD_HOOK

#include <functional>

// An application-owned low-level keyboard hook that captures keys pressed while
// the Windows key is held.
//
// The "Capture system keyboard shortcuts" feature relies on SDL's keyboard grab,
// which installs a WH_KEYBOARD_LL hook. That hook only swallows Win/Alt/Ctrl/
// Tab/Esc and passes every other key through to Windows. Windows consumes the
// Win+<key> system hotkeys (Win+L, Win+R, Win+S, ...) before they are posted to
// the window, so Artemis never receives the non-Win half of the combination and
// has nothing to forward to the host.
//
// This hook is installed after SDL installs its own keyboard grab hook, so it is
// dispatched first. It swallows and forwards those keys itself, mirroring what
// Deskflow/Barrier do with their own keyboard hook.
namespace WinKeyboardHook {

// Install the hook (idempotent). Must be called on a thread that pumps messages
// (the SDL event thread) and after SDL installs its own keyboard grab hook.
void install();

// Remove the hook and release any keys that were captured while installed.
void uninstall();

// Predicate used to decide whether keys may be captured. This is evaluated for
// each candidate key event so the hook follows the same condition as
// SdlInputHandler::isSystemKeyCaptureActive() (e.g. stop capturing as soon as
// the window loses focus, even in fullscreen mode). Set it before install().
void setCaptureCheck(std::function<bool()> check);

} // namespace WinKeyboardHook

#endif // HAVE_WIN_KEYBOARD_HOOK
