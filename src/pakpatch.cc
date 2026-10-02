#include "pakpatch.h"

#include <windows.h>

#include <sddl.h>

#include <cctype>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "detours.h"

#include "pakfile.h"
#include "utils.h"
#include "version.h"

namespace {
#if defined(_M_ARM64)
#define BUILD_ARCH " (ARM64)"
#elif defined(_M_X64)
#define BUILD_ARCH " (64-bit)"
#else
#define BUILD_ARCH " (32-bit)"
#endif

static HANDLE resources_pak_map = nullptr;

static auto RawCreateFileMapping = CreateFileMappingW;
static auto RawMapViewOfFile = MapViewOfFile;

// `PakPatch()` runs in the browser and in every renderer (see the loader in
// chrome++.cc: with in-process WebUI resource loading the renderer serves
// `chrome://settings` from its own pak mapping, so it must patch that mapping
// itself). Re-running the content scan in each renderer inflated every gzip
// entry up to the target and re-deflated the patched one at level 9, on the
// renderer main thread during `PreSandboxStartup` -- a freeze on every new
// tab and cross-site navigation. Instead the browser scans once and hands the
// result to its children through two inherited environment variables: the
// target's pak resource id, so a renderer that has to decompress inflates
// exactly one entry, and the name of a read-only section holding the
// browser's already-patched entry, which a renderer applies with a single
// memcpy and no decompression at all.
constexpr wchar_t kPakTargetIdEnv[] = L"CHROME_PLUS_PAK_RES_ID";
constexpr wchar_t kPakBlobEnv[] = L"CHROME_PLUS_PAK_BLOB";

struct PakBlobHeader {
  uint32_t resource_id;
  uint32_t length;
};

// Keeps the published section alive for the browser's lifetime so child
// processes can open it by name.
static HANDLE published_blob_section = nullptr;

// The loader calls `PakPatch()` only in the browser and in `--type=renderer`
// children (chrome++.cc `Loader`), so no `--type` switch means the browser.
bool IsBrowserProcess() {
  return !GetCommandLineSwitch(L"--type").has_value();
}

uint16_t GetPakTargetId() {
  wchar_t value[8];
  DWORD len = GetEnvironmentVariableW(kPakTargetIdEnv, value, ARRAYSIZE(value));
  if (len == 0 || len >= ARRAYSIZE(value)) {
    return 0;
  }
  wchar_t* end = nullptr;
  unsigned long id = wcstoul(value, &end, 10);
  if (end == value || id == 0 || id > 0xFFFF) {
    return 0;
  }
  return static_cast<uint16_t>(id);
}

// Browser side: copy the patched slot into a named read-only section and
// publish the name through the environment. Read-only is load-bearing: the
// bytes feed the privileged settings WebUI in every renderer, so a writable
// section would be an HTML injection vector into `chrome://settings`. The
// DACL grants read to Everyone and to RestrictedCode (the renderer's
// restricting SID), so a renderer's pre-lockdown token can open the section
// at the same point it maps the pak.
void PublishPatchedEntry(uint8_t* buffer,
                         size_t buffer_size,
                         uint16_t resource_id) {
  // After an in-app restart the new browser inherits the old browser's
  // section name, which dies with that process; drop it and republish under
  // this pid.
  SetEnvironmentVariableW(kPakBlobEnv, nullptr);

  const auto slot = FindResourceSlot(buffer, buffer_size, resource_id);
  if (!slot) {
    return;
  }

  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:(A;;GR;;;WD)(A;;GR;;;RC)", SDDL_REVISION_1,
          &sa.lpSecurityDescriptor, nullptr)) {
    return;
  }

  const std::wstring name =
      L"Local\\ChromePlusPakBlob_" + std::to_wstring(GetCurrentProcessId());
  const DWORD size = static_cast<DWORD>(sizeof(PakBlobHeader) + slot->length);
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                      0, size, name.c_str());
  const DWORD create_error = GetLastError();
  LocalFree(sa.lpSecurityDescriptor);
  if (!section) {
    return;
  }
  // A pre-existing name means another process squatted it; leave children on
  // the decompress fallback rather than trust its contents.
  if (create_error == ERROR_ALREADY_EXISTS) {
    CloseHandle(section);
    return;
  }

  auto* view =
      static_cast<uint8_t*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, size));
  if (!view) {
    CloseHandle(section);
    return;
  }
  auto* header = reinterpret_cast<PakBlobHeader*>(view);
  header->resource_id = resource_id;
  header->length = slot->length;
  memcpy(view + sizeof(PakBlobHeader), buffer + slot->offset, slot->length);
  UnmapViewOfFile(view);

  published_blob_section = section;
  SetEnvironmentVariableW(kPakBlobEnv, name.c_str());
  DebugLog(L"PakPatch: published resource {} ({} bytes) as {}", resource_id,
           slot->length, name);
}

// Renderer fast path: overwrite this process's copy-on-write pak view with
// the browser's already-patched bytes. The slot is re-derived from this
// process's own pak index and the lengths must match, so a section built from
// a different pak is rejected and the copy cannot write outside the slot.
bool ApplyPatchedEntry(uint8_t* buffer, size_t buffer_size) {
  wchar_t name[64];
  DWORD len = GetEnvironmentVariableW(kPakBlobEnv, name, ARRAYSIZE(name));
  if (len == 0 || len >= ARRAYSIZE(name)) {
    return false;
  }

  HANDLE section = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
  if (!section) {
    return false;
  }

  bool applied = false;
  if (const auto* view = static_cast<const uint8_t*>(
          MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0))) {
    const auto* header = reinterpret_cast<const PakBlobHeader*>(view);
    std::optional<PakResourceSlot> slot;
    if (header->resource_id <= 0xFFFF) {
      slot = FindResourceSlot(buffer, buffer_size,
                              static_cast<uint16_t>(header->resource_id));
    }
    if (slot && slot->length == header->length) {
      memcpy(buffer + slot->offset, view + sizeof(PakBlobHeader), slot->length);
      applied = true;
      DebugLog(L"PakPatch: applied published resource {}", header->resource_id);
    }
    UnmapViewOfFile(view);
  }
  CloseHandle(section);
  return applied;
}

// Binding names that mark the update row and its icon container, tried in
// order. Only the *name* matters: Chrome 123 spells them
// `hidden="[[!showUpdateStatus_]]"` /
// `hidden="[[!shouldShowIcons_(showUpdateStatus_)]]"` (Polymer) while current
// Chrome spells them `?hidden="${!this.showUpdateStatus_}"` (Lit), and matching
// on the name handles both.
//
// When a Chrome build renames them, this is the single place to extend:
// `tests/settings_html_patch_test.cc --extract <chrome>\resources.pak` dumps
// the markup that build actually ships, and `--file` shows what the patch made
// of it.
constexpr std::string_view kUpdateStatusBindings[] = {
    "showUpdateStatus_", "showUpdateStatus", "showUpdateStatusVisible_"};
constexpr std::string_view kUpdateIconsBindings[] = {
    "shouldShowIcons_", "shouldShowIcons", "showUpdateIcons_"};

// Forces `hidden="true"` onto every element whose template mentions one of
// `bindings`.
bool ForceHiddenForBindings(std::string& html,
                            const std::string_view (&bindings)[3]);

// Forces `hidden="true"` onto the element whose template mentions `binding`.
//
// The exact-string replacements below only fit the Chrome version they were
// written against. The settings page is served from this pak entry, and a
// frozen older Chrome spells the same binding differently -- e.g.
// `hidden="[[!showUpdateStatus_]]"` instead of
// `?hidden="${!this.showUpdateStatus_}"` -- which left the update row (and its
// "unable to start update check" error) on screen. Matching the binding name
// instead makes the hiding independent of the template syntax. Returns true
// when at least one attribute was rewritten.
bool ForceHiddenForBinding(std::string& html, std::string_view binding) {
  bool changed = false;
  size_t from = 0;
  while (true) {
    const size_t at = html.find(binding, from);
    if (at == std::string::npos) {
      return changed;
    }
    from = at + binding.size();

    // Only rewrite the binding when it sits inside a start tag: the same name
    // also appears in the page's script, which must be left alone.
    const size_t tag_start = html.rfind('<', at);
    const size_t tag_end = html.find('>', at);
    if (tag_start == std::string::npos || tag_end == std::string::npos ||
        tag_start + 1 >= tag_end ||
        !std::isalpha(static_cast<unsigned char>(html[tag_start + 1])) ||
        html.find('>', tag_start) != tag_end) {
      continue;
    }

    // Replace the value of that tag's `hidden` attribute, whatever its
    // spelling: `hidden=`, `?hidden=` (Lit) or `hidden$=` (Polymer).
    size_t pos = tag_start;
    while ((pos = html.find("hidden", pos)) != std::string::npos &&
           pos < tag_end) {
      const bool question_mark = pos > 0 && html[pos - 1] == '?';
      const size_t attr_start = question_mark ? pos - 1 : pos;
      if (attr_start == tag_start ||
          !std::isspace(static_cast<unsigned char>(html[attr_start - 1]))) {
        pos += 6;  // Part of a longer attribute name.
        continue;
      }

      size_t value_start = pos + 6;
      if (value_start < tag_end && html[value_start] == '$') {
        ++value_start;
      }
      while (value_start < tag_end &&
             std::isspace(static_cast<unsigned char>(html[value_start]))) {
        ++value_start;
      }
      if (value_start >= tag_end || html[value_start] != '=') {
        pos += 6;  // A bare `hidden` already hides the element.
        continue;
      }
      ++value_start;
      while (value_start < tag_end &&
             std::isspace(static_cast<unsigned char>(html[value_start]))) {
        ++value_start;
      }

      size_t value_end = value_start;
      if (value_end < tag_end &&
          (html[value_end] == '"' || html[value_end] == '\'')) {
        const char quote = html[value_end];
        const size_t closing = html.find(quote, value_end + 1);
        value_end = (closing == std::string::npos || closing > tag_end)
                        ? tag_end
                        : closing + 1;
      } else {
        while (value_end < tag_end &&
               !std::isspace(static_cast<unsigned char>(html[value_end]))) {
          ++value_end;
        }
      }

      html.replace(attr_start, value_end - attr_start, "hidden=\"true\"");
      changed = true;
      break;
    }
  }
}

bool ForceHiddenForBindings(std::string& html,
                            const std::string_view (&bindings)[3]) {
  bool changed = false;
  for (std::string_view binding : bindings) {
    changed |= ForceHiddenForBinding(html, binding);
  }
  return changed;
}

// The #172 settings-page injection, run on each candidate decompressed pak
// entry until it finds the one holding the settings-about-page HTML.
bool PatchSettingsHtml(uint8_t* begin, uint32_t size, size_t& new_len) {
  BYTE search_start[] = R"(</settings-about-page>)";
  auto match = SearchMemory(
      std::span<uint8_t>(begin, size),
      std::span<const uint8_t>(search_start, sizeof(search_start) - 1));
  if (match.empty()) {
    return false;
  }

  // Compress the HTML for writing patch information.
  std::string html(reinterpret_cast<char*>(begin), size);
  compression_html(html);

  // RemoveUpdateError
  // if (IsNeedPortable())
  bool hid_update_status =
      ReplaceStringInPlace(html, R"(?hidden="${!this.showUpdateStatus_}")",
                           R"(hidden="true")");
  hid_update_status |= ForceHiddenForBindings(html, kUpdateStatusBindings);
  bool hid_icons = ReplaceStringInPlace(
      html, R"(?hidden="${!this.shouldShowIcons_()}")", R"(hidden="true")");
  hid_icons |= ForceHiddenForBindings(html, kUpdateIconsBindings);

  const char product_title[] =
      R"({aboutBrowserVersion}</div><div class="secondary">Powered by <a target="_blank" href="https://github.com/smzhzy26/chrome_plus">Chrome++ Next</a> )" RELEASE_VER_STR BUILD_ARCH
      R"(</div>)";
  const bool injected_version = ReplaceStringInPlace(
      html, R"({aboutBrowserVersion}</div>)", product_title);

  if (!hid_update_status && !hid_icons && !injected_version) {
    // The entry holds the marker but none of the anchors this patch knows
    // about, which means the page markup moved. Leave the resource untouched
    // rather than shipping a whitespace-only rewrite that only looks patched.
    DebugLog(L"PakPatch: settings entry found, nothing to patch");
    return false;
  }
  if (!injected_version) {
    DebugLog(L"PakPatch: settings entry patched without the version banner");
  }

  if (html.length() > size) {
    return false;
  }
  memcpy(begin, html.c_str(), html.length());
  new_len = html.length();
  return true;
}

// One flow per process kind: the browser locates and patches the entry
// itself, then publishes the id and the patched bytes for its children; a
// renderer takes the cheapest tier available -- published bytes (no
// decompression), targeted decompress (one entry), full content scan. Runs
// inside `MyMapViewOfFile` after both hooks have detached themselves, so the
// section create/open/map calls in the publish and apply helpers reach the
// real APIs, not our hooks.
void PatchResourcesPak(uint8_t* buffer, size_t buffer_size) {
  if (!buffer || buffer_size == 0) {
    // Every index offset is validated against the view size, so without it
    // there is nothing safe to scan.
    DebugLog(L"PakPatch: unknown pak view size, skipping");
    return;
  }

  const bool is_browser = IsBrowserProcess();
  if (!is_browser && ApplyPatchedEntry(buffer, buffer_size)) {
    return;
  }

  const uint16_t target_id = GetPakTargetId();
  uint16_t matched_id =
      TraversalGZIPFile(buffer, buffer_size, PatchSettingsHtml, target_id);
  if (matched_id == 0 && target_id != 0) {
    // The inherited id missed, so the pak was replaced (browser updated
    // between sessions); redo the full content scan.
    matched_id = TraversalGZIPFile(buffer, buffer_size, PatchSettingsHtml, 0);
  }

  if (is_browser && matched_id != 0) {
    SetEnvironmentVariableW(kPakTargetIdEnv,
                            std::to_wstring(matched_id).c_str());
    PublishPatchedEntry(buffer, buffer_size, matched_id);
  }
}

HANDLE WINAPI MyMapViewOfFile(_In_ HANDLE hFileMappingObject,
                              _In_ DWORD dwDesiredAccess,
                              _In_ DWORD dwFileOffsetHigh,
                              _In_ DWORD dwFileOffsetLow,
                              _In_ SIZE_T dwNumberOfBytesToMap) {
  if (hFileMappingObject == resources_pak_map) {
    // Modify it to be modifiable.
    LPVOID buffer =
        RawMapViewOfFile(hFileMappingObject, FILE_MAP_COPY, dwFileOffsetHigh,
                         dwFileOffsetLow, dwNumberOfBytesToMap);

    // No more hook needed.
    resources_pak_map = nullptr;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<LPVOID*>(&RawMapViewOfFile),
                 reinterpret_cast<void*>(MyMapViewOfFile));
    auto status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      DebugLog(L"Unhook RawMapViewOfFile failed {}", status);
    }

    if (buffer) {
      // The pak index is validated against the size of the view, which for a
      // file mapping is the region the view spans; a failed query leaves the
      // pak untouched rather than scanned past its end.
      MEMORY_BASIC_INFORMATION view_info = {};
      const size_t view_size =
          ::VirtualQuery(buffer, &view_info, sizeof(view_info)) ==
                  sizeof(view_info)
              ? view_info.RegionSize
              : 0;
      if (view_size == 0) {
        DebugLog(L"PakPatch: VirtualQuery failed: {}", GetLastError());
      }
      PatchResourcesPak(static_cast<uint8_t*>(buffer), view_size);
    }

    return buffer;
  }

  return RawMapViewOfFile(hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh,
                          dwFileOffsetLow, dwNumberOfBytesToMap);
}

// Identify `resources.pak` where it is mapped, by querying the handle, rather
// than where it is opened. Chrome maps the pak with
// `CreateFileMapping`/`MapViewOfFile` (`base::MemoryMappedFile`), so this hook
// catches it directly in every process that loads it -- including the renderer,
// which opens the pak by path itself (`AddDataPackFromPath` in
// `ChromeMainDelegate::PreSandboxStartup`, before sandbox lockdown). The
// previous `CreateFile` hook existed only to record that handle so this hook
// could match it later; querying the handle here drops that second hook.
// `GetFileInformationByHandleEx(FileNameInfo)` reads the path of a handle the
// process already holds, which the sandbox permits -- it brokers new opens
// (`NtCreateFile`/`NtOpenFile`), not operations on existing handles.
// https://chromium.googlesource.com/chromium/src/+/main/docs/design/sandbox.md
bool IsResourcesPak(HANDLE hFile) {
  if (hFile == nullptr || hFile == INVALID_HANDLE_VALUE) {
    return false;
  }

  // An install path can be longer than MAX_PATH, and a buffer that is too
  // small makes this call fail: the pak then goes unmatched and unpatched.
  constexpr size_t kMaxNameBytes = 32 * 1024;
  std::vector<BYTE> buffer(sizeof(FILE_NAME_INFO) +
                           MAX_PATH * sizeof(wchar_t));
  for (;;) {
    if (GetFileInformationByHandleEx(hFile, FileNameInfo, buffer.data(),
                                     static_cast<DWORD>(buffer.size()))) {
      break;
    }
    const DWORD error = GetLastError();
    if ((error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER) ||
        buffer.size() >= kMaxNameBytes) {
      return false;
    }
    const size_t grown = buffer.size() * 2;
    buffer.resize(grown > kMaxNameBytes ? kMaxNameBytes : grown);
  }

  const auto* info = reinterpret_cast<const FILE_NAME_INFO*>(buffer.data());
  std::wstring_view name(info->FileName,
                         info->FileNameLength / sizeof(wchar_t));
  return name.ends_with(L"resources.pak");
}

HANDLE WINAPI MyCreateFileMapping(_In_ HANDLE hFile,
                                  _In_opt_ LPSECURITY_ATTRIBUTES lpAttributes,
                                  _In_ DWORD flProtect,
                                  _In_ DWORD dwMaximumSizeHigh,
                                  _In_ DWORD dwMaximumSizeLow,
                                  _In_opt_ LPCTSTR lpName) {
  if (IsResourcesPak(hFile)) {
    // Force copy-on-write so the mapped view can be patched in memory.
    resources_pak_map =
        RawCreateFileMapping(hFile, lpAttributes, PAGE_WRITECOPY,
                             dwMaximumSizeHigh, dwMaximumSizeLow, lpName);

    // No more hook needed.
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<LPVOID*>(&RawCreateFileMapping),
                 reinterpret_cast<void*>(MyCreateFileMapping));
    auto status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      DebugLog(L"Unhook RawCreateFileMapping failed {}", status);
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<LPVOID*>(&RawMapViewOfFile),
                 reinterpret_cast<void*>(MyMapViewOfFile));
    status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      DebugLog(L"Hook RawMapViewOfFile failed {}", status);
    }

    return resources_pak_map;
  }
  return RawCreateFileMapping(hFile, lpAttributes, flProtect, dwMaximumSizeHigh,
                              dwMaximumSizeLow, lpName);
}

}  // namespace

void PakPatch() {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(reinterpret_cast<LPVOID*>(&RawCreateFileMapping),
               reinterpret_cast<void*>(MyCreateFileMapping));
  auto status = DetourTransactionCommit();
  if (status != NO_ERROR) {
    DebugLog(L"Hook RawCreateFileMapping failed {}", status);
  }
}
