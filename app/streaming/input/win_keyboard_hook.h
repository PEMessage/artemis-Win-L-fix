#pragma once

#ifdef HAVE_WIN_KEYBOARD_HOOK

#include <functional>

// An application-owned low-level keyboard hook that captures the Windows key and
// keys pressed while it is held.
//
// The "Capture system keyboard shortcuts" feature relies on SDL's keyboard grab,
// which installs a WH_KEYBOARD_LL hook. That hook only swallows Win/Alt/Ctrl/
// Tab/Esc and passes every other key through to Windows. Windows consumes the
// Win+<key> system hotkeys (Win+L, Win+R, Win+S, ...) before they are posted to
// the window, so Artemis never receives the non-Win half of the combination and
// has nothing to forward to the host.
//
// This hook is installed after SDL installs its own keyboard grab hook, so it is
// dispatched first. While system key capture is active it takes ownership of the
// Windows key (swallows it and forwards it together with the combination), so
// the combination is forwarded in the right order and the Windows key is always
// released again. This mirrors what Deskflow/Barrier do with their own hook.
namespace WinKeyboardHook {

// Install the hook (idempotent). Must be called on a thread that pumps messages
// (the SDL event thread) and after SDL installs its own keyboard grab hook.
void install();

// Re-install the hook so that it is dispatched before SDL's grab hook again.
//
// SDL re-installs its own keyboard grab hook whenever the window regains focus
// (SDL_UpdateWindowGrab()), which would otherwise put SDL's hook in front of
// ours and hide the Windows key from us. Keeps the forwarded key state, so no
// keys are released.
void reinstall();

// Remove the hook and release any keys that were captured while installed.
void uninstall();

// Release the Windows key and any captured keys the host may still be holding.
// Call this whenever input capture is interrupted (e.g. the window lost focus)
// so the host never ends up with stuck keys.
void releaseKeys();

// Predicate used to decide whether keys may be captured. This is evaluated for
// each candidate key event so the hook follows the same condition as
// SdlInputHandler::isSystemKeyCaptureActive() (e.g. stop capturing as soon as
// the window loses focus, even in fullscreen mode). Set it before install().
void setCaptureCheck(std::function<bool()> check);

} // namespace WinKeyboardHook

#endif // HAVE_WIN_KEYBOARD_HOOK
