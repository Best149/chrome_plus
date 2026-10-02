#include "utils.h"

#include <windows.h>

#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Global variable definitions
HMODULE hInstance = nullptr;

// Global constants - use functions to avoid static initialization order issues
const std::wstring& GetAppPath() {
  static std::wstring app_path = []() {
    // `GetModuleFileNameW` truncates silently when the buffer is too small and
    // only signals it through the return value, so grow until the path fits.
    // Everything below (the ini path, the portable data/cache directories, the
    // `%app%` substitution, the AppUserModelID) is derived from this value, and
    // a truncated path would silently point at the wrong directory.
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
      const DWORD length = ::GetModuleFileNameW(
          nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
      if (length == 0) {
        return std::wstring();
      }
      if (length < buffer.size() || buffer.size() >= 0x8000) {
        // Either it fits, or the API's own 32767-character limit is reached.
        return std::wstring(buffer.data(), length);
      }
      buffer.resize(buffer.size() * 2);
    }
  }();
  return app_path;
}

const std::wstring& GetAppDir() {
  static std::wstring app_dir = []() {
    const std::wstring app_path = GetAppPath();
    if (app_path.empty()) {
      return app_path;
    }
    // `PathRemoveFileSpecW` edits in place and takes no length, so keep it off
    // a fixed-size buffer.
    std::vector<wchar_t> buffer(app_path.begin(), app_path.end());
    buffer.push_back(L'\0');
    ::PathRemoveFileSpecW(buffer.data());
    return std::wstring(buffer.data());
  }();
  return app_dir;
}

const std::wstring& GetIniPath() {
  static std::wstring ini_path = GetAppDir() + L"\\chrome++.ini";
  return ini_path;
}

// String manipulation functions
// Specify the delimiter and wrapper to split the string.
std::vector<std::wstring> StringSplit(std::wstring_view str,
                                      const wchar_t delim,
                                      std::wstring_view enclosure) {
  std::vector<std::wstring> result;
  auto parts = std::views::split(str, delim);
  for (const auto& part : parts) {
    std::wstring_view part_sv(part);
    if (!enclosure.empty()) {
      if (!part_sv.empty() && part_sv.front() == enclosure.front()) {
        part_sv.remove_prefix(1);
      }
      if (!part_sv.empty() && part_sv.back() == enclosure.back()) {
        part_sv.remove_suffix(1);
      }
    }
    result.emplace_back(part_sv);
  }
  return result;
}

std::vector<std::string> StringSplit(std::string_view str,
                                     const char delim,
                                     std::string_view enclosure) {
  std::vector<std::string> result;
  auto parts = std::views::split(str, delim);
  for (const auto& part : parts) {
    std::string_view part_sv(part);
    if (!enclosure.empty()) {
      if (!part_sv.empty() && part_sv.front() == enclosure.front()) {
        part_sv.remove_prefix(1);
      }
      if (!part_sv.empty() && part_sv.back() == enclosure.back()) {
        part_sv.remove_suffix(1);
      }
    }
    result.emplace_back(part_sv);
  }
  return result;
}

// Compression html.
std::string& ltrim(std::string& s) {
  auto it = std::ranges::find_if_not(
      s, [](unsigned char c) { return std::isspace(c); });
  s.erase(s.begin(), it);
  return s;
}

std::string& rtrim(std::string& s) {
  auto reversed_view = s | std::views::reverse;
  auto it = std::ranges::find_if_not(
      reversed_view, [](unsigned char c) { return std::isspace(c); });
  s.erase(it.base(), s.end());
  return s;
}

std::string& trim(std::string& s) {
  return ltrim(rtrim(s));
}

void compression_html(std::string& html) {
  auto lines = StringSplit(html, '\n');
  html.clear();
  for (auto& line : lines) {
    html += "\n";
    html += trim(line);
  }
}

bool ReplaceStringInPlace(std::string& subject,
                          std::string_view search,
                          std::string_view replace) {
  bool find = false;
  size_t pos = 0;
  while ((pos = subject.find(search, pos)) != std::string::npos) {
    subject.replace(pos, search.length(), replace);
    pos += replace.length();
    find = true;
  }
  return find;
}

bool ReplaceStringInPlace(std::wstring& subject,
                          std::wstring_view search,
                          std::wstring_view replace) {
  bool find = false;
  size_t pos = 0;
  while ((pos = subject.find(search, pos)) != std::wstring::npos) {
    subject.replace(pos, search.length(), replace);
    pos += replace.length();
    find = true;
  }
  return find;
}

std::wstring QuoteSpaceIfNeeded(const std::wstring& str) {
  if (!str.contains(L' ')) {
    return str;
  }

  std::wstring escaped(L"\"");
  for (auto c : str) {
    if (c == L'"') {
      escaped += L'"';
    }
    escaped += c;
  }
  escaped += L'"';
  return escaped;
}

std::wstring JoinArgsString(const std::vector<std::wstring>& lines,
                            std::wstring_view delimiter) {
  if (lines.empty()) {
    return L"";
  }
  return lines | std::views::transform(QuoteSpaceIfNeeded) |
         std::views::join_with(delimiter) | std::ranges::to<std::wstring>();
}

// Search memory.
std::span<uint8_t> SearchMemory(std::span<uint8_t> src,
                                std::span<const uint8_t> sub) {
  if (src.empty() || sub.empty() || src.size() < sub.size()) {
    return {};
  }
  auto it = std::search(src.begin(), src.end(),
                        std::boyer_moore_searcher(sub.begin(), sub.end()));
  if (it != src.end()) {
    return src.subspan(std::distance(src.begin(), it));
  }
  return {};
}

std::wstring GetIniString(std::wstring_view section,
                          std::wstring_view key,
                          std::wstring_view default_value) {
  std::vector<TCHAR> buffer(100);
  DWORD bytesread = 0;
  do {
    bytesread = ::GetPrivateProfileStringW(
        section.data(), key.data(), default_value.data(), buffer.data(),
        static_cast<DWORD>(buffer.size()), GetIniPath().c_str());
    if (bytesread >= buffer.size() - 1) {
      buffer.resize(buffer.size() * 2);
    } else {
      break;
    }
  } while (true);

  return std::wstring(buffer.data());
}

namespace {

// Resolves `path` to a full path, collapsing "." and ".." -- what
// `PathCanonicalize` did here. `GetFullPathNameW` leaves the buffer undefined
// when it fails and reports the required size when it truncates, so a fixed
// `MAX_PATH` buffer could otherwise hand back uninitialized bytes or a
// silently truncated path for long inputs. The caller keeps its own value when
// resolution fails.
std::wstring ResolvePath(const std::wstring& path) {
  if (path.empty()) {
    return path;
  }

  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD length = ::GetFullPathNameW(
        path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0) {
      DebugLog(L"Path resolution failed for '{}': {}", path, ::GetLastError());
      return path;
    }
    if (length < buffer.size()) {
      return std::wstring(buffer.data(), length);
    }
    // The return value is the required size, including the null terminator.
    buffer.resize(length);
  }
}

}  // namespace

std::wstring CanonicalizePath(const std::wstring& path) {
  return ResolvePath(path);
}

std::wstring GetAbsolutePath(const std::wstring& path) {
  return ResolvePath(path);
}

std::wstring ExpandEnvironmentPath(const std::wstring& path) {
  std::vector<wchar_t> buffer(MAX_PATH);
  size_t ExpandedLength = ::ExpandEnvironmentStrings(
      path.c_str(), &buffer[0], static_cast<DWORD>(buffer.size()));
  if (ExpandedLength > buffer.size()) {
    buffer.resize(ExpandedLength);
    ExpandedLength = ::ExpandEnvironmentStrings(
        path.c_str(), &buffer[0], static_cast<DWORD>(buffer.size()));
  }
  return std::wstring(&buffer[0], 0, ExpandedLength);
}

HWND GetTopWnd(HWND hwnd) {
  while (::GetParent(hwnd) && ::IsWindowVisible(::GetParent(hwnd))) {
    hwnd = ::GetParent(hwnd);
  }
  return hwnd;
}

void ExecuteCommand(int id, HWND hwnd) {
  if (hwnd == 0) {
    hwnd = GetForegroundWindow();
  }
  // A null hwnd would turn the PostMessage below into a thread message.
  if (!hwnd) {
    return;
  }
  // Browser commands are window-scoped. Callers often pass the HWND under the
  // cursor (child widget / render host); route to the top-level frame.
  if (const HWND root = ::GetAncestor(hwnd, GA_ROOT)) {
    hwnd = root;
  }
  // Post, do not Send: frequently called from the UI-thread mouse hook
  // (double-click close). A synchronous SendMessageTimeout can re-enter
  // Chrome while the hook is still on the stack and break the next
  // window gesture after close.
  ::PostMessageW(hwnd, WM_SYSCOMMAND, id, 0);
}

namespace {

// Launches `cmd.exe /c <command>` detached. `launch_on_exit` runs from
// `DllMain(DLL_PROCESS_DETACH)`, where the CRT and the console subsystem may
// already be torn down: `_wsystem` both depends on them and blocks until the
// child exits, which holds up teardown. `start` inside the command line runs
// the real program asynchronously, so nothing here has to wait for it.
void SpawnDetachedCommand(const std::wstring& command) {
  std::wstring command_line = L"cmd.exe /c " + command;
  std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
  buffer.emplace_back(L'\0');

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info = {};
  if (::CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup_info,
                       &process_info)) {
    ::CloseHandle(process_info.hThread);
    ::CloseHandle(process_info.hProcess);
    return;
  }
  DebugLog(L"LaunchCommands: failed to launch '{}': {}", command,
           ::GetLastError());
}

}  // namespace

void LaunchCommands(const std::wstring& get_commands) {
  auto commands = StringSplit(
      get_commands,
      L';');  // Quotes should not be used as they can cause errors with paths
              // that contain spaces. Since semicolons rarely appear in names
              // and commands, they are used as delimiters.
  if (commands.empty()) {
    return;
  }
  for (const auto& command : commands) {
    std::wstring expanded_path = ExpandEnvironmentPath(command);
    ReplaceStringInPlace(expanded_path, L"%app%", GetAppDir());

    // Using `start` launches the command in a new window asynchronously,
    // avoiding blocking Chrome's main thread. For more details:
    // https://github.com/Bush2021/chrome_plus/issues/130#issuecomment-2925782726
    // https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/start
    //  `cmd /c` ensures the command window exits after execution, preventing
    //  the "Not enough memory resources are available to process this command"
    //  error even when all commands run successfully.
    std::wstring cmd =
        LR"(start "chrome++ cmd" cmd /c ")" + expanded_path + LR"(")";
    SpawnDetachedCommand(cmd);
  }
}

[[nodiscard]] bool IsChromeWindow(HWND hwnd) {
  std::array<wchar_t, 256> class_name_buffer{};
  const int length =
      ::GetClassNameW(hwnd, class_name_buffer.data(),
                      static_cast<int>(class_name_buffer.size()));
  if (length == 0) {
    return false;
  }
  const std::wstring_view class_name_view{class_name_buffer.data(),
                                          static_cast<std::size_t>(length)};
  constexpr std::wstring_view target_prefix = L"Chrome_WidgetWin_";
  return class_name_view.starts_with(target_prefix);
}

std::optional<std::wstring> GetCommandLineSwitch(std::wstring_view name) {
  if (name.empty()) {
    return std::nullopt;
  }

  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) {
    return std::nullopt;
  }

  std::optional<std::wstring> value;
  // argv[0] is the executable path.
  for (int i = 1; i < argc && !value.has_value(); ++i) {
    const std::wstring_view arg(argv[i]);
    if (!arg.starts_with(name)) {
      continue;
    }
    if (arg.size() == name.size()) {
      value = std::wstring();
    } else if (arg[name.size()] == L'=') {
      value = std::wstring(arg.substr(name.size() + 1));
    }
  }
  ::LocalFree(argv);
  return value;
}

namespace {

// Modifier keys mapping
constexpr std::pair<std::wstring_view, UINT> kModifierKeys[] = {
    {L"shift", MOD_SHIFT},     {L"ctrl", MOD_CONTROL},
    {L"control", MOD_CONTROL},  // alias
    {L"alt", MOD_ALT},         {L"win", MOD_WIN},
};

// Special virtual keys mapping
constexpr std::pair<std::wstring_view, UINT> kSpecialKeys[] = {
    // Arrow keys
    {L"left", VK_LEFT},
    {L"right", VK_RIGHT},
    {L"up", VK_UP},
    {L"down", VK_DOWN},
    {L"←", VK_LEFT},
    {L"→", VK_RIGHT},
    {L"↑", VK_UP},
    {L"↓", VK_DOWN},
    // Control keys
    {L"esc", VK_ESCAPE},
    {L"escape", VK_ESCAPE},  // alias
    {L"tab", VK_TAB},
    {L"backspace", VK_BACK},
    {L"enter", VK_RETURN},
    {L"return", VK_RETURN},  // alias
    {L"space", VK_SPACE},
    // System keys
    {L"prtsc", VK_SNAPSHOT},
    {L"printscreen", VK_SNAPSHOT},  // alias
    {L"scroll", VK_SCROLL},
    {L"pause", VK_PAUSE},
    // Navigation keys
    {L"insert", VK_INSERT},
    {L"delete", VK_DELETE},
    {L"del", VK_DELETE},  // alias
    {L"home", VK_HOME},
    {L"end", VK_END},
    {L"pageup", VK_PRIOR},
    {L"pgup", VK_PRIOR},  // alias
    {L"pagedown", VK_NEXT},
    {L"pgdn", VK_NEXT},  // alias
};

constexpr bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::towlower(a[i]) != std::towlower(b[i]))  // case-insensitive
      return false;
  }
  return true;
}

template <size_t N>
constexpr std::optional<UINT> FindInKeyMap(
    std::wstring_view key,
    const std::pair<std::wstring_view, UINT> (&map)[N]) {
  for (const auto& [name, code] : map) {
    if (EqualsIgnoreCase(key, name))
      return code;
  }
  return std::nullopt;
}

// Parse function key (F1-F24)
std::optional<UINT> ParseFunctionKey(std::wstring_view key) {
  if (key.size() < 2 || (key[0] != L'F' && key[0] != L'f'))
    return std::nullopt;

  auto num_part = key.substr(1);
  if (num_part.empty() || !std::ranges::all_of(num_part, ::iswdigit))
    return std::nullopt;

  int fx = 0;
  for (wchar_t c : num_part) {
    fx = fx * 10 + (c - L'0');
  }

  if (fx >= 1 && fx <= 24)
    return VK_F1 + fx - 1;
  return std::nullopt;
}

// Parse single character key (A-Z, 0-9, symbols)
std::optional<UINT> ParseCharacterKey(std::wstring_view key) {
  if (key.size() != 1)
    return std::nullopt;

  wchar_t ch = key[0];
  if (std::iswalnum(ch))
    return static_cast<UINT>(std::towupper(ch));

  // For other characters, use `VkKeyScan`
  SHORT scan = ::VkKeyScanW(ch);
  if (scan != -1)
    return LOBYTE(scan);

  return std::nullopt;
}

}  // namespace

UINT ParseHotkeys(std::wstring_view keys, bool no_repeat) {
  UINT modifiers = 0;
  UINT virtual_key = 0;

  for (const auto& part : std::views::split(keys, L'+')) {
    std::wstring_view key(part.begin(), part.end());
    if (key.empty())
      continue;
    if (auto mod = FindInKeyMap(key, kModifierKeys)) {
      modifiers |= *mod;
      continue;
    }
    if (auto vk = FindInKeyMap(key, kSpecialKeys)) {
      virtual_key = *vk;
      continue;
    }
    if (auto vk = ParseFunctionKey(key)) {
      virtual_key = *vk;
      continue;
    }
    if (auto vk = ParseCharacterKey(key)) {
      virtual_key = *vk;
      continue;
    }
    // Unknown token. Rejecting the whole combination (0 is never a valid
    // result: the key code occupies the high word) keeps a typo such as
    // `Ctrl+Shfit+A` from silently registering `Ctrl+A` instead.
    return 0;
  }

  if (virtual_key == 0)
    return 0;

  if (no_repeat)
    modifiers |= MOD_NOREPEAT;

  return MAKELPARAM(modifiers, virtual_key);
}
