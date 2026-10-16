#include "win_keyboard_hook.h"

#ifdef HAVE_WIN_KEYBOARD_HOOK

#include <cstring>

// minwindef.h defines min()/max() macros that can conflict with other headers.
#define NOMINMAX

#include "SDL_compat.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <Limelight.h>

namespace {

HHOOK s_hook = nullptr;

// VK code of the Windows key we currently forward to the host, or 0 if none.
//
// While system key capture is active this hook owns the Windows key: it swallows
// VK_LWIN/VK_RWIN and forwards them itself. That guarantees
//   * the Windows key down reaches the host before any Win+<key> combination,
//   * the key-up is always forwarded (no stuck Windows key on the host), and
//   * Windows never sees the Windows key at all, so it cannot open the local
//     Start menu or trigger local Win+<key> hotkeys (including Win+L).
//
// All state here is driven purely by the key events we see; we deliberately do
// NOT use GetAsyncKeyState() to decide whether the Windows key is held, because
// inside a low-level hook it can return a stale value and would make us swallow
// keys (for example the Ctrl+Alt+Shift+Z capture toggle) that we must not.
DWORD s_winKeyVk = 0;

// Keys that we swallowed (and forwarded ourselves) because they were pressed
// while we owned the Windows key.
bool s_capturedKeys[256];
int s_capturedKeyCount = 0;

// Set by the input handler. Evaluated per key so we stop capturing as soon as
// system key capture is no longer active (e.g. the window lost focus).
std::function<bool()> s_captureCheck;

bool captureIsActive()
{
    return s_captureCheck && s_captureCheck();
}

// Keys that SDL's own grab hook already handles, plus the modifier keys. These
// must be allowed to reach SDL so that the normal input path keeps the state
// (m_KeysDown / modifier flags) in sync.
bool isHandledBySdl(DWORD vk)
{
    switch (vk) {
    case VK_LWIN:
    case VK_RWIN:
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_TAB:
    case VK_ESCAPE:
        return true;
    default:
        return false;
    }
}

// Modifiers are intentionally sent as 0. The modifier keys themselves are
// forwarded as their own key events by the normal input path, and all known GFE
// versions drop any keyboard event that carries the META flag, so the flags add
// nothing here and are safer left out.
void forwardKey(DWORD vk, bool down)
{
    LiSendKeyboardEvent2(static_cast<short>(0x8000 | vk),
                         down ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                         0, 0);
}

void releaseWinKey()
{
    if (s_winKeyVk != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Windows keyboard hook: forward Windows key up (vk=0x%02lX)",
                    static_cast<unsigned long>(s_winKeyVk));
        forwardKey(s_winKeyVk, false);
        s_winKeyVk = 0;
    }
}

void raiseCapturedKeys()
{
    for (int vk = 0; vk < 256; vk++) {
        if (s_capturedKeys[vk]) {
            s_capturedKeys[vk] = false;
            s_capturedKeyCount--;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Windows keyboard hook: release captured key (vk=0x%02X)",
                        vk);
            forwardKey(vk, false);
        }
    }
}

LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        // Ignore events we (or another tool) injected.
        if ((info->flags & LLKHF_INJECTED) == 0) {
            const DWORD vk = info->vkCode;
            const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
            const bool capturing = captureIsActive();

            // If system key capture stopped while we were holding the Windows
            // key or a combination key, release them now so the host cannot end
            // up with stuck keys.
            if (!capturing && (s_winKeyVk != 0 || s_capturedKeyCount != 0)) {
                raiseCapturedKeys();
                releaseWinKey();
            }

            if (vk == VK_LWIN || vk == VK_RWIN) {
                if (!capturing) {
                    // Not capturing system keys: leave the Windows key to the
                    // normal (SDL grab) path as before.
                    return CallNextHookEx(s_hook, code, wParam, lParam);
                }

                if (down) {
                    if (s_winKeyVk == 0) {
                        // Take ownership of the Windows key for this hold.
                        s_winKeyVk = vk;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "Windows keyboard hook: forward Windows key down (vk=0x%02lX)",
                                    static_cast<unsigned long>(vk));
                        forwardKey(vk, true);
                    }

                    // Windows must never see the Windows key while we own it.
                    return 1;
                }

                if (s_winKeyVk == vk) {
                    s_winKeyVk = 0;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Windows keyboard hook: forward Windows key up (vk=0x%02lX)",
                                static_cast<unsigned long>(vk));
                    forwardKey(vk, false);
                    return 1;
                }

                // We did not forward a key-down for this key (it was pressed
                // while capture was off, or SDL's grab hook saw it first). Pass
                // the key-up through so the normal input path forwards the
                // matching release. Swallowing it here would leave the host with
                // a stuck Windows key.
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Windows keyboard hook: pass Windows key up (vk=0x%02lX) to normal path",
                            static_cast<unsigned long>(vk));
                return CallNextHookEx(s_hook, code, wParam, lParam);
            }

            if (vk < 256 && s_capturedKeys[vk]) {
                // We swallowed the press, so always deliver the release (even if
                // the Windows key was already released) to avoid a stuck key.
                if (up) {
                    s_capturedKeys[vk] = false;
                    s_capturedKeyCount--;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Windows keyboard hook: forward key up (vk=0x%02lX)",
                                static_cast<unsigned long>(vk));
                    forwardKey(vk, false);
                }

                // Swallow auto-repeat events and the release.
                return 1;
            }

            if (down && capturing && s_winKeyVk != 0 && vk < 256 && !isHandledBySdl(vk)) {
                // Windows would treat this as a Win+<key> system hotkey and
                // never post it to our window, so capture and forward it
                // ourselves.
                s_capturedKeys[vk] = true;
                s_capturedKeyCount++;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Windows keyboard hook: forward key down (vk=0x%02lX)",
                            static_cast<unsigned long>(vk));
                forwardKey(vk, true);
                return 1;
            }
        }
    }

    return CallNextHookEx(s_hook, code, wParam, lParam);
}

} // namespace

namespace WinKeyboardHook {

void install()
{
    if (s_hook != nullptr) {
        return;
    }

    s_winKeyVk = 0;
    s_capturedKeyCount = 0;
    std::memset(s_capturedKeys, 0, sizeof(s_capturedKeys));

    // NB: WH_KEYBOARD_LL callbacks are dispatched on the thread that installed
    // the hook, so this must be called from a thread with a message loop (the
    // SDL event thread). A NULL module handle is valid for low-level hooks.
    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, nullptr, 0);
    if (s_hook == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Windows keyboard hook: failed to install: %lu",
                     static_cast<unsigned long>(GetLastError()));
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Windows keyboard hook: installed");
    }
}

void uninstall()
{
    if (s_hook != nullptr) {
        UnhookWindowsHookEx(s_hook);
        s_hook = nullptr;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Windows keyboard hook: uninstalled");
    }

    releaseKeys();
}

void reinstall()
{
    if (s_hook == nullptr) {
        return;
    }

    // Re-hook to move us back to the front of the hook chain. We keep
    // s_winKeyVk / s_capturedKeys so nothing is released mid-hold.
    UnhookWindowsHookEx(s_hook);
    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, nullptr, 0);
    if (s_hook == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Windows keyboard hook: failed to re-install: %lu",
                     static_cast<unsigned long>(GetLastError()));
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Windows keyboard hook: re-installed");
    }
}

void releaseKeys()
{
    raiseCapturedKeys();
    releaseWinKey();
}

void setCaptureCheck(std::function<bool()> check)
{
    s_captureCheck = check;
}

} // namespace WinKeyboardHook

#endif // HAVE_WIN_KEYBOARD_HOOK
