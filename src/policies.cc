#include "policies.h"

#include <windows.h>

#include <shlwapi.h>

#include "detours.h"

#include "config.h"
#include "utils.h"

namespace {

static auto RawRegOpenKeyExW = RegOpenKeyExW;

bool IsPolicyKey(LPCWSTR lpSubKey) {
  if (!lpSubKey) {
    return false;
  }
  if (StrStrIW(lpSubKey, L"Policies\\Google\\Chrome") ||
      StrStrIW(lpSubKey, L"Policies\\Microsoft\\Edge") ||
      StrStrIW(lpSubKey, L"Policies\\Chromium") ||
      StrStrIW(lpSubKey, L"Policies\\BraveSoftware\\Brave")) {
    return true;
  }
  return false;
}

LSTATUS APIENTRY MyRegOpenKeyExW(HKEY hKey,
                                 LPCWSTR lpSubKey,
                                 DWORD ulOptions,
                                 REGSAM samDesired,
                                 PHKEY phkResult) {
  // It is `HKEY_LOCAL_MACHINE` on my computer, but just in case. `HKEY_USERS`
  // is covered too: a hive root that is not listed here would still hand the
  // policy keys out.
  if ((hKey == HKEY_LOCAL_MACHINE || hKey == HKEY_CURRENT_USER ||
       hKey == HKEY_USERS) &&
      IsPolicyKey(lpSubKey)) {
    return ERROR_FILE_NOT_FOUND;
  }
  return RawRegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
}

}  // namespace

void IgnorePolicies() {
  if (!config.IsIgnorePolicies()) {
    return;
  }

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(reinterpret_cast<LPVOID*>(&RawRegOpenKeyExW),
               reinterpret_cast<void*>(MyRegOpenKeyExW));
  auto status = DetourTransactionCommit();
  if (status != NO_ERROR) {
    DebugLog(L"IgnorePolicies failed: {}", status);
  } else {
    DebugLog(L"IgnorePolicies enabled.");
  }
}
