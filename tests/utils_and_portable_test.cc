// Offline checks for the parts that decide what the relaunched browser gets and
// how paths/keys are parsed: `GetCommand` in src/portable.cc (which assembles
// the portable command line), the path helpers in src/utils.cc, and
// `ParseHotkeys`.
//
//   chrome_plus_utils_test
//
// Note: this includes the implementation directly, so src/portable.cc must not
// be compiled into the same target (see the CHROME_PLUS_BUILD_TESTS block in
// CMakeLists.txt).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "../src/portable.cc"  // real GetCommand (anonymous namespace)
#include "../src/utils.h"

namespace {

int failures = 0;

void Expect(const char* group, bool condition, const char* what) {
  if (condition) {
    std::printf("  ok   %s: %s\n", group, what);
  } else {
    std::printf("  FAIL %s: %s\n", group, what);
    ++failures;
  }
}

bool Contains(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring_view::npos;
}

size_t CountOccurrences(std::wstring_view haystack, std::wstring_view needle) {
  size_t count = 0;
  for (size_t at = haystack.find(needle); at != std::wstring_view::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

void PrintResult(const char* name, const std::wstring& command) {
  std::wprintf(L"  [%hs] %s\n", name, command.c_str());
}

void RunPortableCommandCases() {
  const char* group = "portable-command";

  // The portable switch is appended by this code, and the switches the
  // injection needs are merged into the single --disable-features Chrome wants.
  {
    const std::wstring command =
        GetCommand(const_cast<LPWSTR>(L"chrome.exe --disable-features=A \"https://example.com/?q=-type=renderer\""));
    PrintResult("features+url", command);
    Expect(group, Contains(command, L"--portable"),
           "--portable appended");
    Expect(group, CountOccurrences(command, L"--portable") == 1,
           "--portable appended exactly once");
    Expect(group, Contains(command, L"--disable-features=A,WinSboxNoFakeGdiInit"),
           "user features merged with the required one");
    Expect(group, CountOccurrences(command, L"--disable-features=") == 1,
           "only one --disable-features survives");
    Expect(group, Contains(command, L"-type=renderer"),
           "an argument that looks like a switch is preserved as a value");
  }

  // --user-data-dir from the command line must win over the configured one.
  {
    const std::wstring command =
        GetCommand(const_cast<LPWSTR>(L"chrome.exe --user-data-dir=C:\\odd --disk-cache-dir=C:\\cc"));
    PrintResult("explicit dirs", command);
    Expect(group, Contains(command, L"--user-data-dir=C:\\odd"),
           "explicit --user-data-dir kept");
    Expect(group, CountOccurrences(command, L"--user-data-dir=") == 1,
           "no second --user-data-dir injected");
    Expect(group, Contains(command, L"--disk-cache-dir=C:\\cc"),
           "explicit --disk-cache-dir kept");
    Expect(group, CountOccurrences(command, L"--disk-cache-dir=") == 1,
           "no second --disk-cache-dir injected");
  }

  // Everything after the `--` sentinel belongs to the page/command and has to
  // come back verbatim, after the switches.
  {
    const std::wstring command =
        GetCommand(const_cast<LPWSTR>(L"chrome.exe --disable-features=B -- --keep \"C:\\a b\\c.html\""));
    PrintResult("sentinel", command);
    Expect(group, Contains(command, L"-- --keep \"C:\\a b\\c.html\""),
           "sentinel arguments preserved verbatim at the end");
    Expect(group, command.ends_with(L"--keep \"C:\\a b\\c.html\""),
           "nothing is appended after them");
  }

  // The Windows shell passes a file association target as one argument through
  // --single-argument; splitting it would break paths with spaces.
  {
    const std::wstring command = GetCommand(const_cast<LPWSTR>(
        L"chrome.exe --single-argument \"C:\\dir with space\\file.html\""));
    PrintResult("single-argument", command);
    Expect(group,
           command.ends_with(L"--single-argument \"C:\\dir with space\\file.html\""),
           "--single-argument suffix untouched");
  }

  // Arguments that gained quotes must stay quoted once.
  {
    const std::wstring command =
        GetCommand(const_cast<LPWSTR>(L"chrome.exe \"C:\\Program Files\\a.html\""));
    PrintResult("quoted arg", command);
    Expect(group, Contains(command, L"\"C:\\Program Files\\a.html\""),
           "argument with a space stays quoted");
  }
}

void RunPathCases() {
  const char* group = "paths";

  Expect(group, GetAppPath().size() > 0 && GetAppPath() != GetAppDir(),
         "GetAppPath returns the module path and GetAppDir its directory");
  Expect(group, GetAppDir() + L"\\chrome++.ini" == GetIniPath(),
         "the ini path is derived from the application directory");

  Expect(group, CanonicalizePath(L"C:\\a\\b\\..\\c") == L"C:\\a\\c",
         "CanonicalizePath collapses `..`");
  Expect(group, CanonicalizePath(L"") .empty(),
         "an empty path is returned as-is");

  // Regression for the fixed MAX_PATH buffers: an over-long path used to come
  // back truncated or as uninitialized stack bytes.
  std::wstring long_path = L"C:\\";
  for (int i = 0; i < 40; ++i) {
    long_path += L"directory-";
  }
  long_path += L"file.txt";
  const std::wstring resolved = GetAbsolutePath(long_path);
  std::wprintf(L"  [long path] %zu characters in, %zu out\n", long_path.size(),
               resolved.size());
  Expect(group, long_path.size() > MAX_PATH, "the test path is longer than MAX_PATH");
  Expect(group, resolved == long_path,
         "GetAbsolutePath returns the full path, not a truncated one");
}

void RunHotkeyCases() {
  const char* group = "hotkeys";

  const UINT shift_a = ParseHotkeys(L"Ctrl+Shift+A");
  Expect(group, HIWORD(shift_a) == 'A' &&
                    (LOWORD(shift_a) & MOD_CONTROL) != 0 &&
                    (LOWORD(shift_a) & MOD_SHIFT) != 0,
         "modifiers and key are decoded");
  Expect(group, (LOWORD(shift_a) & MOD_NOREPEAT) != 0,
         "no-repeat is applied by default");
  Expect(group, ParseHotkeys(L"Ctrl+Shift+A", false) ==
                    (shift_a & ~static_cast<UINT>(MOD_NOREPEAT)),
         "no_repeat=false leaves the flag out");
  Expect(group, ParseHotkeys(L"Ctrl+Shfit+A") == 0,
         "a misspelled modifier rejects the whole combination");
  Expect(group, ParseHotkeys(L"Ctrl") == 0, "a modifier without a key is rejected");
  Expect(group, ParseHotkeys(L"Ctrl+Shift+") == 0, "a trailing plus is rejected");
  Expect(group, HIWORD(ParseHotkeys(L"F24")) == VK_F24, "F24 parses");
  Expect(group, ParseHotkeys(L"F25") == 0, "F25 is out of range");
  Expect(group, HIWORD(ParseHotkeys(L"PageDown")) == VK_NEXT, "named keys parse");
}

void RunStringCases() {
  const char* group = "strings";

  // The enclosure only strips a matching leading/trailing quote from each part;
  // it is not a quoting mechanism, which is why a tab name containing the
  // delimiter cannot be expressed (documented in chrome++.ini).
  const auto names = StringSplit(L"\"about:blank\",\"新建标签\"", L',', L"\"");
  Expect(group, names.size() == 2 && names[0] == L"about:blank" &&
                    names[1] == L"新建标签",
         "the default new-tab names split and lose their quotes");
  const auto quoted_comma = StringSplit(L"\"a,b\",c", L',', L"\"");
  Expect(group, quoted_comma.size() == 3,
         "an embedded delimiter still splits (known behaviour)");

  std::wstring subject = L"%app%\\x %app%";
  Expect(group, ReplaceStringInPlace(subject, L"%app%", L"C:\\c") &&
                    subject == L"C:\\c\\x C:\\c",
         "ReplaceStringInPlace replaces every occurrence");

  const std::vector<std::wstring> args = {L"--a=1", L"C:\\Program Files\\x"};
  const std::wstring joined = JoinArgsString(args, L" ");
  Expect(group, joined == L"--a=1 \"C:\\Program Files\\x\"",
         "JoinArgsString quotes only what needs quoting");

  std::string html = "  <div>\n    <span>x</span>\n";
  compression_html(html);
  Expect(group, html.find("<div>") != std::string::npos &&
                    html.find("  <div>") == std::string::npos,
         "compression_html trims every line");
}

}  // namespace

int wmain() {
  std::printf("portable command line\n");
  RunPortableCommandCases();
  std::printf("paths\n");
  RunPathCases();
  std::printf("hotkeys\n");
  RunHotkeyCases();
  std::printf("strings\n");
  RunStringCases();
  std::printf("\n%s (%d failure(s))\n",
              failures == 0 ? "ALL PASSED" : "FAILURES", failures);
  return failures == 0 ? 0 : 1;
}
