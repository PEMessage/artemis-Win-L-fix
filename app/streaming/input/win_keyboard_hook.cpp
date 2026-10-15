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

// True while the Windows key (either one) is physically held down. Tracked from
// the events we see in this hook, which runs before SDL's own grab hook.
bool s_winKeyDown = false;

// Keys that we swallowed (and forwarded ourselves) because they were pressed
// while the Windows key was held. Tracked so we can always deliver the matching
// key-up, even if the Windows key is released first.
bool s_capturedKeys[256];

// Set by the input handler. Evaluated per key so we stop capturing as soon as
// system key capture is no longer active (e.g. the window lost focus).
std::function<bool()> s_captureCheck;

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

// Mirrors the modifier flags SdlInputHandler::handleKeyEvent() would have sent,
// except for MODIFIER_META: the Windows key is forwarded as its own key event
// (VK_LWIN) by the normal input path, and all known GFE versions drop any
// keyboard event that carries the META flag. Leaving it out keeps Win+<key>
// working on both GFE and Sunshine/Apollo.
unsigned char getModifiers()
{
    unsigned char modifiers = 0;

    if ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0) {
        modifiers |= MODIFIER_SHIFT;
    }
    if ((GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0) {
        modifiers |= MODIFIER_CTRL;
    }
    if ((GetAsyncKeyState(VK_LMENU) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0) {
        modifiers |= MODIFIER_ALT;
    }

    return modifiers;
}

void raiseCapturedKeys()
{
    for (int vk = 0; vk < 256; vk++) {
        if (s_capturedKeys[vk]) {
            s_capturedKeys[vk] = false;
            LiSendKeyboardEvent2(0x8000 | vk, KEY_ACTION_UP, 0, 0);
        }
    }
}

LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        const DWORD vk = info->vkCode;
        const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

        if (vk == VK_LWIN || vk == VK_RWIN) {
            // Let SDL's grab hook capture and forward the Windows key itself.
            // We only need to remember that it is down.
            s_winKeyDown = down;
        }
        else if (vk < 256 && s_capturedKeys[vk]) {
            // We swallowed the press, so always deliver the release (even if the
            // Windows key has already been released) to avoid a stuck key.
            if (up) {
                s_capturedKeys[vk] = false;
                LiSendKeyboardEvent2(0x8000 | vk, KEY_ACTION_UP, getModifiers(), 0);
            }

            // Swallow auto-repeat events and the release.
            return 1;
        }
        else if (down && s_winKeyDown && vk < 256 && !isHandledBySdl(vk) &&
                 s_captureCheck && s_captureCheck()) {
            // Windows would treat this as a Win+<key> system hotkey and never
            // post it to our window, so capture and forward it ourselves.
            s_capturedKeys[vk] = true;
            LiSendKeyboardEvent2(0x8000 | vk, KEY_ACTION_DOWN, getModifiers(), 0);
            return 1;
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

    s_winKeyDown = false;
    std::memset(s_capturedKeys, 0, sizeof(s_capturedKeys));

    // NB: WH_KEYBOARD_LL callbacks are dispatched on the thread that installed
    // the hook, so this must be called from a thread with a message loop (the
    // SDL event thread). A NULL module handle is valid for low-level hooks.
    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, nullptr, 0);
    if (s_hook == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to install Windows keyboard hook: %lu",
                     static_cast<unsigned long>(GetLastError()));
    }
}

void uninstall()
{
    if (s_hook != nullptr) {
        UnhookWindowsHookEx(s_hook);
        s_hook = nullptr;
    }

    raiseCapturedKeys();
    s_winKeyDown = false;
}

void setCaptureCheck(std::function<bool()> check)
{
    s_captureCheck = check;
}

} // namespace WinKeyboardHook

#endif // HAVE_WIN_KEYBOARD_HOOK
