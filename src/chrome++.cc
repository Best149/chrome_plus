#include <windows.h>

#include <psapi.h>
#include <stdio.h>

#include "detours.h"

#include "appid.h"
#include "config.h"
#include "green.h"
#include "hijack.h"
#include "hotkey.h"
#include "inputhook.h"
#include "keymapping.h"
#include "pakpatch.h"
#include "policies.h"
#include "portable.h"
#include "tabbookmark.h"
#include "upgradenotification.h"
#include "utils.h"
#include "version.h"

using Startup = int (*)();
static bool should_run_exit_cmd = false;
Startup ExeMain = nullptr;

void ChromePlus() {
  // Shortcut.
  SetAppId();

  // Portable hijack patch.
  MakeGreen();

  // Ignore enterprise policies.
  IgnorePolicies();

  // Initialize key mapping and translate key
  KeyMapping();

  // Enhancement of the address bar, tab, and bookmark.
  TabBookmark();

  // Install input hooks (must be called after all handlers are registered).
  InstallInputHooks();

  // Patch the pak file.
  PakPatch();

  // Suppress Chrome's false "out of date" upgrade notification.
  SuppressFalseUpgradeNotification();

  // Process the hotkey.
  GetHotkey();
}

void ChromePlusCommand(LPWSTR param) {
  // A relaunched portable instance carries the switch appended below. Parsing
  // the switch instead of searching the raw command line keeps a URL or path
  // that happens to contain the same text from skipping the relaunch.
  if (!GetCommandLineSwitch(L"--portable").has_value()) {
    // Exits inside the call on success. When the relaunch fails the other
    // features are deliberately left off: this process would otherwise run the
    // user's default profile without the portable directory overrides the
    // configuration asked for.
    Portable(param);
    return;
  }

  ChromePlus();
  LaunchCommands(config.GetLaunchOnStartup());
  should_run_exit_cmd = true;
}

int Loader() {
  // Only main interface.
  LPWSTR param = GetCommandLineW();
  // DebugLog(L"param {}", param);
  const std::optional<std::wstring> process_type =
      GetCommandLineSwitch(L"--type");
  if (!process_type.has_value()) {
    ChromePlusCommand(param);
  } else if (*process_type == L"renderer") {
    // With in-process WebUI resource loading on (V1, crrev.com/c/5868139,
    // crbug.com/362511750), the browser fills `LocalResourceLoaderConfig`
    // (content/browser/webui/web_ui_impl.cc) and the renderer materializes
    // WebUI from its own inherited-handle `resources.pak`
    // (ui/base/resource/data_pack.cc), not over IPC. So the renderer must
    // patch that mapping itself to keep the injection from #172. We stopped
    // forcing V1 off because the field trial enabling
    // `InitialWebUISyncNavStartToCommit` (crrev.com/c/7778247) then made the
    // renderer CHECK that config (content/renderer/render_frame_impl.cc) and
    // crash (#263). Other sub-process types never serve WebUI, so skip them.
    PakPatch();
  }

  // Return to the main function.
  return ExeMain();
}

void InstallLoader() {
  const HMODULE module = GetModuleHandle(nullptr);
  // Get the address of the original entry point of the main module.
  MODULEINFO mi = {};
  if (!module || !GetModuleInformation(GetCurrentProcess(), module, &mi,
                                       sizeof(mi)) ||
      !mi.EntryPoint) {
    // Without a valid entry point the trampoline address would be garbage, so
    // leave the process alone rather than detouring an arbitrary address.
    DebugLog(L"InstallLoader: entry point lookup failed: {}", GetLastError());
    return;
  }
  ExeMain = reinterpret_cast<Startup>(mi.EntryPoint);

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  const LONG attach_error = DetourAttach(
      reinterpret_cast<LPVOID*>(&ExeMain), reinterpret_cast<void*>(Loader));
  const LONG status = DetourTransactionCommit();
  if (attach_error != NO_ERROR || status != NO_ERROR) {
    DebugLog(L"InstallLoader failed: attach={}, commit={}", attach_error,
             status);
  }
}

__declspec(dllexport) void portable() {}

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID pv) {
  if (dwReason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    hInstance = hModule;

    // Maintain the original function of system DLLs.
    LoadSysDll(hModule);

    InstallLoader();
  } else if (dwReason == DLL_PROCESS_DETACH && ::should_run_exit_cmd) {
    LaunchCommands(config.GetLaunchOnExit());
    should_run_exit_cmd = false;
  }
  return TRUE;
}
