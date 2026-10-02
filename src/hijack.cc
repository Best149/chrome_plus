#include "hijack.h"

#include <windows.h>

#include <intrin.h>
#include <psapi.h>
#include <stdint.h>

#include <cstring>

#include "detours.h"

#include "utils.h"

#define NOP_FUNC        \
  {                     \
    __nop();            \
    __nop();            \
    __nop();            \
    __nop();            \
    __nop();            \
    __nop();            \
    __nop();            \
    __nop();            \
    __nop();            \
    __nop();            \
    __nop();            \
    __nop();            \
    return __COUNTER__; \
  }

#define EXPORT(api) int __cdecl api() NOP_FUNC

#pragma comment( \
    linker,      \
    "/export:GetFileVersionInfoA=?GetFileVersionInfoA@hijack@@YAHXZ,@1")
#pragma comment( \
    linker,      \
    "/export:GetFileVersionInfoByHandle=?GetFileVersionInfoByHandle@hijack@@YAHXZ,@2")
#pragma comment( \
    linker,      \
    "/export:GetFileVersionInfoExA=?GetFileVersionInfoExA@hijack@@YAHXZ,@3")
#pragma comment( \
    linker,      \
    "/export:GetFileVersionInfoExW=?GetFileVersionInfoExW@hijack@@YAHXZ,@4")
#pragma comment( \
    linker,      \
    "/export:GetFileVersionInfoSizeA=?GetFileVersionInfoSizeA@hijack@@YAHXZ,@5")
#pragma comment( \
    linker,      \
    "/export:GetFileVersionInfoSizeExA=?GetFileVersionInfoSizeExA@hijack@@YAHXZ,@6")
#pragma comment( \
    linker,      \
    "/export:GetFileVersionInfoSizeExW=?GetFileVersionInfoSizeExW@hijack@@YAHXZ,@7")
#pragma comment( \
    linker,      \
    "/export:GetFileVersionInfoSizeW=?GetFileVersionInfoSizeW@hijack@@YAHXZ,@8")
#pragma comment( \
    linker,      \
    "/export:GetFileVersionInfoW=?GetFileVersionInfoW@hijack@@YAHXZ,@9")
#pragma comment(linker, "/export:VerFindFileA=?VerFindFileA@hijack@@YAHXZ,@10")
#pragma comment(linker, "/export:VerFindFileW=?VerFindFileW@hijack@@YAHXZ,@11")
#pragma comment(linker, \
                "/export:VerInstallFileA=?VerInstallFileA@hijack@@YAHXZ,@12")
#pragma comment(linker, \
                "/export:VerInstallFileW=?VerInstallFileW@hijack@@YAHXZ,@13")
#pragma comment( \
    linker, "/export:VerLanguageNameA=?VerLanguageNameA@hijack@@YAHXZ,@14")
#pragma comment( \
    linker, "/export:VerLanguageNameW=?VerLanguageNameW@hijack@@YAHXZ,@15")
#pragma comment(linker, \
                "/export:VerQueryValueA=?VerQueryValueA@hijack@@YAHXZ,@16")
#pragma comment(linker, \
                "/export:VerQueryValueW=?VerQueryValueW@hijack@@YAHXZ,@17")

// Make it globally visible
namespace hijack {
EXPORT(GetFileVersionInfoA)
EXPORT(GetFileVersionInfoByHandle)
EXPORT(GetFileVersionInfoExA)
EXPORT(GetFileVersionInfoExW)
EXPORT(GetFileVersionInfoSizeA)
EXPORT(GetFileVersionInfoSizeExA)
EXPORT(GetFileVersionInfoSizeExW)
EXPORT(GetFileVersionInfoSizeW)
EXPORT(GetFileVersionInfoW)
EXPORT(VerFindFileA)
EXPORT(VerFindFileW)
EXPORT(VerInstallFileA)
EXPORT(VerInstallFileW)
EXPORT(VerLanguageNameA)
EXPORT(VerLanguageNameW)
EXPORT(VerQueryValueA)
EXPORT(VerQueryValueW)
}  // namespace hijack

namespace {
// Internal helper functions are kept in the anonymous namespace
void InstallDetours(PBYTE pTarget, PBYTE pDetour) {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  const LONG attach_error =
      DetourAttach(&reinterpret_cast<PVOID&>(pTarget), pDetour);
  const LONG status = DetourTransactionCommit();
  if (attach_error != NO_ERROR || status != NO_ERROR) {
    DebugLog(L"LoadSysDll: hook failed: attach={}, commit={}", attach_error,
             status);
  }
}

void LoadVersion(HINSTANCE module_handle) {
  auto image_base = reinterpret_cast<PBYTE>(module_handle);
  auto dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(image_base);
  if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
    return;
  }

  // Every structure below is located through a file-provided offset, so each
  // one is checked against the mapped image before it is dereferenced: a bad
  // `e_lfanew` or an absent export directory would otherwise be read as a
  // table of names and functions.
  MODULEINFO module_info = {};
  if (!GetModuleInformation(GetCurrentProcess(), module_handle, &module_info,
                            sizeof(module_info)) ||
      module_info.SizeOfImage == 0) {
    DebugLog(L"LoadSysDll: module information unavailable: {}", GetLastError());
    return;
  }
  const DWORD image_size = module_info.SizeOfImage;

  const DWORD nt_offset = static_cast<DWORD>(dos_header->e_lfanew);
  if (nt_offset < sizeof(IMAGE_DOS_HEADER) || nt_offset > image_size ||
      image_size - nt_offset < sizeof(IMAGE_NT_HEADERS)) {
    return;
  }

  auto nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(image_base + nt_offset);
  if (nt_headers->Signature != IMAGE_NT_SIGNATURE) {
    return;
  }

  const IMAGE_DATA_DIRECTORY& export_data =
      nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (export_data.VirtualAddress == 0 ||
      export_data.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
      export_data.VirtualAddress > image_size ||
      image_size - export_data.VirtualAddress < export_data.Size) {
    return;
  }

  auto export_directory = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(
      image_base + export_data.VirtualAddress);

  // The three tables have to live inside the export directory blob.
  const auto within_export_directory = [&](DWORD rva, size_t bytes) {
    return rva >= export_data.VirtualAddress && bytes <= export_data.Size &&
           rva - export_data.VirtualAddress <= export_data.Size - bytes;
  };

  if (!within_export_directory(
          export_directory->AddressOfNames,
          static_cast<size_t>(export_directory->NumberOfNames) *
              sizeof(DWORD)) ||
      !within_export_directory(export_directory->AddressOfFunctions,
                               static_cast<size_t>(
                                   export_directory->NumberOfFunctions) *
                                   sizeof(DWORD)) ||
      !within_export_directory(export_directory->AddressOfNameOrdinals,
                               static_cast<size_t>(
                                   export_directory->NumberOfNames) *
                                   sizeof(WORD))) {
    return;
  }

  auto name_table = reinterpret_cast<DWORD*>(
      image_base + export_directory->AddressOfNames);
  auto function_table = reinterpret_cast<DWORD*>(
      image_base + export_directory->AddressOfFunctions);
  auto ordinals_table = reinterpret_cast<WORD*>(
      image_base + export_directory->AddressOfNameOrdinals);

  wchar_t system_directory[MAX_PATH + 1];
  if (!GetSystemDirectory(system_directory, MAX_PATH)) {
    return;
  }

  wchar_t dll_path[MAX_PATH + 1];
  lstrcpy(dll_path, system_directory);
  lstrcat(dll_path, TEXT("\\version.dll"));

  HINSTANCE original_dll_handle = LoadLibrary(dll_path);
  if (!original_dll_handle) {
    return;
  }

  for (DWORD i = 0; i < export_directory->NumberOfNames; ++i) {
    const DWORD name_rva = name_table[i];
    if (name_rva < export_data.VirtualAddress ||
        name_rva >= export_data.VirtualAddress + export_data.Size) {
      continue;
    }
    const size_t name_room =
        export_data.Size - (name_rva - export_data.VirtualAddress);
    auto* function_name = reinterpret_cast<const char*>(image_base + name_rva);
    // The name has to be NUL-terminated inside the directory to be usable.
    if (std::memchr(function_name, '\0', name_room) == nullptr) {
      continue;
    }

    const WORD ordinal = ordinals_table[i];
    if (ordinal >= export_directory->NumberOfFunctions) {
      continue;
    }
    const DWORD function_rva = function_table[ordinal];
    // A forwarded export points at an "OtherDll.Function" string inside the
    // directory, and `GetProcAddress` resolves it into another module's code;
    // that target must not be patched from here.
    if (function_rva >= export_data.VirtualAddress &&
        function_rva < export_data.VirtualAddress + export_data.Size) {
      continue;
    }

    auto original_function = reinterpret_cast<PBYTE>(
        GetProcAddress(original_dll_handle, function_name));
    if (original_function) {
      InstallDetours(image_base + function_rva, original_function);
    }
  }
}
}  // namespace

void LoadSysDll(HINSTANCE hModule) {
  LoadVersion(hModule);
}