#include "inputhook.h"

#include <windows.h>

#include <algorithm>
#include <vector>

#include "utils.h"

namespace {

template <typename Handler>
struct HandlerEntry {
  Handler handler;
  int priority;
};

std::vector<HandlerEntry<KeyboardHandler>> keyboard_handlers;
std::vector<HandlerEntry<MouseHandler>> mouse_handlers;

HHOOK keyboard_hook = nullptr;
HHOOK mouse_hook = nullptr;

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION) {
    // `WH_KEYBOARD` receives only flag bits in `lParam`, not the
    // `KBDLLHOOKSTRUCT` the low-level hook gets, so the `dwExtraInfo` marker
    // that `SendKey`/`SendMappedKey` attach cannot be read from the event
    // itself; the queue's extra info carries that marker instead (same value
    // `MOUSEHOOKSTRUCT::dwExtraInfo` exposes to `MouseProc`). Without this
    // check a key mapping whose target is another mapping's source -- or its
    // own source -- re-enters these handlers and recurses, and `Ctrl+W` sent
    // by a mapping is re-interpreted by the tab handlers. Clearing the marker
    // keeps it from outliving the injected event.
    if (::GetMessageExtraInfo() == static_cast<LPARAM>(GetMagicCode())) {
      ::SetMessageExtraInfo(0);
      return CallNextHookEx(keyboard_hook, nCode, wParam, lParam);
    }

    for (const auto& entry : keyboard_handlers) {
      if (entry.handler(wParam, lParam)) {
        return 1;
      }
    }
  }
  return CallNextHookEx(keyboard_hook, nCode, wParam, lParam);
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode != HC_ACTION) {
    return CallNextHookEx(mouse_hook, nCode, wParam, lParam);
  }

  if (wParam == WM_NCMOUSEMOVE) {
    return CallNextHookEx(mouse_hook, nCode, wParam, lParam);
  }

  PMOUSEHOOKSTRUCT pmouse = reinterpret_cast<PMOUSEHOOKSTRUCT>(lParam);

  if (pmouse->dwExtraInfo == GetMagicCode()) {
    return CallNextHookEx(mouse_hook, nCode, wParam, lParam);
  }

  for (const auto& entry : mouse_handlers) {
    if (entry.handler(wParam, lParam)) {
      return 1;
    }
  }

  return CallNextHookEx(mouse_hook, nCode, wParam, lParam);
}

}  // namespace

void RegisterKeyboardHandler(KeyboardHandler handler,
                             HandlerPriority priority) {
  keyboard_handlers.emplace_back(std::move(handler), static_cast<int>(priority));
  std::ranges::sort(keyboard_handlers, [](const auto& a, const auto& b) {
    return a.priority < b.priority;
  });
}

void RegisterMouseHandler(MouseHandler handler, HandlerPriority priority) {
  mouse_handlers.emplace_back(std::move(handler), static_cast<int>(priority));
  std::ranges::sort(mouse_handlers, [](const auto& a, const auto& b) {
    return a.priority < b.priority;
  });
}

bool IsKeyPressed(int vk) {
  return vk && (::GetKeyState(vk) & 0x8000) != 0;
}

void InstallInputHooks() {
  // Both hooks are thread-scoped to Chrome's UI thread, so the module handle
  // only identifies the DLL that owns the procedures.
  keyboard_hook = SetWindowsHookEx(WH_KEYBOARD, KeyboardProc, hInstance,
                                   GetCurrentThreadId());
  if (!keyboard_hook) {
    DebugLog(L"SetWindowsHookEx(WH_KEYBOARD) failed: {}", GetLastError());
  }

  mouse_hook =
      SetWindowsHookEx(WH_MOUSE, MouseProc, hInstance, GetCurrentThreadId());
  if (!mouse_hook) {
    DebugLog(L"SetWindowsHookEx(WH_MOUSE) failed: {}", GetLastError());
  }
}
