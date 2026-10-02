// Offline checks for the settings-about-page patch in src/pakpatch.cc.
//
// Whether that patch matches is a property of the Chrome build on disk: it
// rewrites the localized about-page HTML inside resources.pak, whose markup
// differs between versions (Polymer vs Lit bindings, renamed properties). This
// tool keeps that checkable without launching the browser:
//
//   chrome_plus_markup_test
//       Runs the built-in markup cases.
//
//   chrome_plus_markup_test --extract <resources.pak> [out.html]
//       Dumps the about-page entry of that Chrome build (default output:
//       about_page.html next to the executable).
//
//   chrome_plus_markup_test --file <about_page.html>
//       Runs the real patch against an extracted entry and reports what it did.
//
// Typical use when moving to a new Chrome version:
//   1. chrome_plus_markup_test --extract "<chrome>\resources.pak"
//   2. chrome_plus_markup_test --file about_page.html
//   3. If the row is not hidden, add the binding name the page actually uses to
//      the candidate lists in src/pakpatch.cc and re-run step 2.
//
// Note: this includes the implementation directly so the anonymous-namespace
// functions can be exercised; that is also why the target must not compile
// src/pakpatch.cc separately (see the CHROME_PLUS_BUILD_TESTS block in
// CMakeLists.txt).
#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "../src/pakfile.h"
#include "../src/pakpatch.cc"  // the real implementation
#include "../src/utils.h"

namespace {

int failures = 0;

void Expect(const char* name, bool condition, const char* what) {
  if (condition) {
    std::printf("  ok   %s: %s\n", name, what);
  } else {
    std::printf("  FAIL %s: %s\n", name, what);
    ++failures;
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

// Start tags that mention `binding`, skipping mentions in script text -- the
// same distinction the patch itself makes. Used to report what a real entry
// looks like before and after the patch.
std::vector<std::string_view> TagMentions(std::string_view document,
                                          std::string_view binding,
                                          size_t limit) {
  std::vector<std::string_view> tags;
  for (size_t at = document.find(binding);
       at != std::string_view::npos && tags.size() < limit;
       at = document.find(binding, at + binding.size())) {
    const size_t begin = document.rfind('<', at);
    const size_t end = document.find('>', at);
    if (begin == std::string_view::npos || end == std::string_view::npos ||
        begin + 1 >= end ||
        !std::isalpha(static_cast<unsigned char>(document[begin + 1])) ||
        document.find('>', begin) != end) {
      continue;
    }
    tags.push_back(document.substr(begin, end - begin + 1));
  }
  return tags;
}

size_t CountOccurrences(std::string_view haystack, std::string_view needle) {
  size_t count = 0;
  for (size_t at = haystack.find(needle); at != std::string_view::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

// Builds a settings-about-page entry around the two rows under test. The
// filler gives the whitespace compression room to make space for the banner.
std::string MakePage(const std::string& update_row, const std::string& icon_row,
                     const std::string& extra) {
  std::string page =
      "<!DOCTYPE html><html><head><style>\n"
      "      .update-status { color: red; }\n"
      "    </style></head><body>\n"
      "  <settings-about-page>\n"
      "    <div id=\"version\">${aboutBrowserVersion}</div>\n";
  page += update_row;
  page += icon_row;
  page += extra;
  page +=
      "    <script>\n"
      "      this.showUpdateStatus_ = true;\n"
      "      this.shouldShowIcons_ = true;\n"
      "      this.otherRow_ = false;\n"
      "    </script>\n";
  for (int i = 0; i < 40; ++i) {
    page +=
        "          <div class=\"cr-row filler\">\n"
        "            <span>filler row</span>\n"
        "          </div>\n";
  }
  page += "  </settings-about-page>\n</body></html>\n";
  return page;
}

// Runs the real patch over `page` and returns the patched bytes.
std::string Patch(std::string page, bool& patched, size_t& new_len) {
  std::vector<uint8_t> buffer(page.begin(), page.end());
  patched = PatchSettingsHtml(buffer.data(),
                              static_cast<uint32_t>(page.size()), new_len);
  if (!patched) {
    return {};
  }
  return std::string(reinterpret_cast<char*>(buffer.data()), new_len);
}

// Asserts the update row and the icons row end up hidden and the banner is
// injected, for one spelling of the bindings.
void Case(const char* name, const std::string& update_row,
          const std::string& icon_row, const char* gone, const char* survives) {
  std::printf("case %s\n", name);
  const std::string page = MakePage(update_row, icon_row, survives ? survives : "");
  bool patched = false;
  size_t new_len = 0;
  const std::string out = Patch(page, patched, new_len);

  Expect(name, patched, "PatchSettingsHtml reported a patch");
  if (!patched) {
    return;
  }
  Expect(name, new_len <= page.size(), "output fits the original entry");
  Expect(name, Contains(out, "class=\"secondary\">Powered by") &&
                   Contains(out, "Chrome++ Next</a>"),
         "version banner injected");
  Expect(name, Contains(out, "id=\"update-status\" hidden=\"true\""),
         "update-status element hidden");
  Expect(name, Contains(out, "id=\"icons\" hidden=\"true\""),
         "icons element hidden");
  Expect(name, Contains(out, "this.showUpdateStatus_ = true;"),
         "script text untouched");
  if (gone) {
    Expect(name, !Contains(out, gone), "bound attribute replaced");
  }
  if (survives) {
    Expect(name, Contains(out, survives), "unrelated markup untouched");
  }
}

// Verbatim excerpt of the update row from Chrome 123.0.6312.123's
// resources.pak (`--extract` on that build's resources.pak), with the row's
// parts, the version line and the entry marker as that build ships them. Note
// the attribute values that continue onto the next line: the parsing has to
// follow a quoted value across lines. The filler at the end stands in for the
// whitespace the real entry carries, which is what makes room for the banner.
//
// Chrome 124.0.6367.61 ships this region unchanged: extracting both builds and
// comparing the 29 lines around `id="updateStatusMessage"` shows no difference,
// and `--file` hides the same two elements in each. So this excerpt covers both
// versions.
constexpr char kChrome123Excerpt[] = R"(<div class="cr-row two-line">
<div class="icon-container" hidden="[[!shouldShowIcons_(showUpdateStatus_)]]">
<iron-icon icon$="[[getUpdateStatusIcon_(
obsoleteSystemInfo_, currentUpdateStatusEvent_)]]" src="[[getThrobberSrcIfUpdating_(
obsoleteSystemInfo_, currentUpdateStatusEvent_)]]">
</iron-icon>
</div>
<div class="flex cr-padded-text">
<div id="updateStatusMessage" hidden="[[!showUpdateStatus_]]">
<div role="alert" aria-live="polite" inner-h-t-m-l="[[getUpdateStatusMessage_(
currentUpdateStatusEvent_)]]">
</div>
<a hidden$="[[!shouldShowLearnMoreLink_(
currentUpdateStatusEvent_)]]" target="_blank" href="https://support.google.com/chrome?p=update_error" aria-label="$i18nPolymer{aboutLearnMoreUpdatingErrors}">
$i18n{learnMore}
</a>
</div>
<span id="deprecationWarning" hidden="[[!obsoleteSystemInfo_.obsolete]]">
$i18n{aboutObsoleteSystem}
</span>
<div class="secondary">$i18n{aboutBrowserVersion}</div>
</div>
          <div class="filler">
            <span>filler row</span>
          </div>
          <div class="filler">
            <span>filler row</span>
          </div>
          <div class="filler">
            <span>filler row</span>
          </div>
          <div class="filler">
            <span>filler row</span>
          </div>
          <div class="filler">
            <span>filler row</span>
          </div>
          <div class="filler">
            <span>filler row</span>
          </div>
          <div class="filler">
            <span>filler row</span>
          </div>
          <div class="filler">
            <span>filler row</span>
          </div>
          <div class="filler">
            <span>filler row</span>
          </div>
          <div class="filler">
            <span>filler row</span>
          </div>
</settings-about-page>)";

void RunBuiltInCases() {
  Case("lit-property",
       "<div id=\"update-status\" ?hidden=\"${!this.showUpdateStatus_}\">\n",
       "<div id=\"icons\" ?hidden=\"${!this.shouldShowIcons_()}\">\n",
       "${!this.showUpdateStatus_}", nullptr);

  // Polymer, which is what Chrome 123 on this machine shipped.
  Case("polymer-chrome-123",
       "<div id=\"update-status\" hidden=\"[[!showUpdateStatus_]]\">\n",
       "<div id=\"icons\" hidden=\"[[!shouldShowIcons_]]\">\n",
       "[[!showUpdateStatus_]]",
       "<div class=\"hidden-row\" hidden=\"[[!otherRow_]]\">x</div>\n");

  Case("lit-method",
       "<div id=\"update-status\" ?hidden=\"${!this.showUpdateStatus_()}\">\n",
       "<div id=\"icons\" ?hidden=\"${!this.shouldShowIcons_()}\">\n",
       "${!this.showUpdateStatus_()}", nullptr);

  Case("polymer-attr-binding",
       "<div id=\"update-status\" hidden$='[[!showUpdateStatus_]]'>\n",
       "<div id=\"icons\" hidden$='[[!shouldShowIcons_]]'>\n",
       "[[!showUpdateStatus_]]", nullptr);

  Case("unquoted",
       "<div id=\"update-status\" hidden=[[!showUpdateStatus_]]>\n",
       "<div id=\"icons\" hidden=[[!shouldShowIcons_]]>\n",
       "[[!showUpdateStatus_]]", nullptr);

  // A row this patch has no binding for must be left alone; the banner still
  // applies, which is what tells a user the patch ran at all.
  std::printf("case %s\n", "unknown-binding");
  {
    const std::string page =
        MakePage("<div id=\"update-status\">error text</div>\n",
                 "<div class=\"hidden-row\" hidden=\"[[!otherRow_]]\">x</div>\n",
                 "");
    bool patched = false;
    size_t new_len = 0;
    const std::string out = Patch(page, patched, new_len);
    Expect("unknown-binding", patched, "PatchSettingsHtml reported a patch");
    Expect("unknown-binding",
           Contains(out, "class=\"secondary\">Powered by"),
           "version banner injected");
    Expect("unknown-binding",
           Contains(out, "<div id=\"update-status\">error text</div>"),
           "row without a known binding left alone");
    Expect("unknown-binding", Contains(out, "[[!otherRow_]]"),
           "unrelated binding untouched");
  }

  // The real Chrome 123 markup -- 124.0.6367.61 ships the same region -- which
  // is what this patch was extended for: the
  // bindings are the same names, spelled the Polymer way, and the icon
  // container's binding even takes the other one as an argument.
  std::printf("case %s\n", "chrome-123-124-real-excerpt");
  {
    const std::string page = kChrome123Excerpt;
    bool patched = false;
    size_t new_len = 0;
    const std::string out = Patch(page, patched, new_len);
    Expect("chrome-123-124", patched, "PatchSettingsHtml reported a patch");
    Expect("chrome-123-124", new_len <= page.size(), "output fits the original entry");
    Expect("chrome-123-124",
           Contains(out, "class=\"icon-container\" hidden=\"true\""),
           "icon container hidden");
    Expect("chrome-123-124",
           Contains(out, "id=\"updateStatusMessage\" hidden=\"true\""),
           "update message row hidden");
    Expect("chrome-123-124", !Contains(out, "[[!showUpdateStatus_]]"),
           "the binding that kept the row visible is gone");
    Expect("chrome-123-124",
           Contains(out, "hidden=\"[[!obsoleteSystemInfo_.obsolete]]\""),
           "unrelated binding untouched");
    // The learn-more link's attribute binding continues onto the next line,
    // which the patch's attribute scan must not mistake for the tag's end.
    Expect("chrome-123-124",
           Contains(out, "hidden$=\"[[!shouldShowLearnMoreLink_("),
           "unrelated multi-line attribute binding untouched");
    Expect("chrome-123-124", Contains(out, "Chrome++ Next</a>"),
           "version banner injected");
  }
}

std::vector<uint8_t> ReadWholeFile(const std::wstring& path, bool& ok) {
  ok = false;
  std::vector<uint8_t> data;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    std::printf("cannot open %ls (error %lu)\n", path.c_str(), GetLastError());
    return data;
  }
  LARGE_INTEGER size = {};
  if (GetFileSizeEx(file, &size) && size.QuadPart > 0) {
    data.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read,
                  nullptr) &&
         read == data.size();
  }
  CloseHandle(file);
  return data;
}

bool WriteWholeFile(const std::wstring& path, const uint8_t* data, size_t size) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    std::printf("cannot create %ls (error %lu)\n", path.c_str(), GetLastError());
    return false;
  }
  DWORD written = 0;
  const bool ok = WriteFile(file, data, static_cast<DWORD>(size), &written,
                            nullptr) &&
                  written == size;
  CloseHandle(file);
  return ok;
}

// Dumps the pak entry holding the about page. The callback returns true to stop
// the walk; the patch's own write-back that follows is irrelevant here because
// the bytes were already written out.
int ExtractAboutPage(const std::wstring& pak_path, const std::wstring& out_path) {
  bool ok = false;
  std::vector<uint8_t> pak = ReadWholeFile(pak_path, ok);
  if (!ok) {
    return 1;
  }

  bool dumped = false;
  constexpr std::string_view kMarker = "</settings-about-page>";
  TraversalGZIPFile(
      pak.data(), pak.size(),
      [&](uint8_t* begin, uint32_t length, size_t&) {
        if (!Contains(std::string_view(reinterpret_cast<char*>(begin), length),
                      kMarker)) {
          return false;
        }
        dumped = WriteWholeFile(out_path, begin, length);
        return dumped;
      },
      0);

  if (!dumped) {
    std::printf("no entry with %.*s found in %ls\n",
                static_cast<int>(kMarker.size()), kMarker.data(),
                pak_path.c_str());
    return 1;
  }
  std::printf("wrote %ls\n", out_path.c_str());
  return 0;
}

// Runs the real patch over an extracted entry and reports the update row before
// and after, so a version whose markup moved is obvious at a glance.
int CheckExtractedFile(const std::wstring& html_path) {
  bool ok = false;
  std::vector<uint8_t> file = ReadWholeFile(html_path, ok);
  if (!ok) {
    return 1;
  }

  std::string before(reinterpret_cast<char*>(file.data()), file.size());
  std::vector<uint8_t> buffer(file.begin(), file.end());
  size_t new_len = 0;
  const bool patched =
      PatchSettingsHtml(buffer.data(), static_cast<uint32_t>(file.size()),
                        new_len);
  std::string after = patched
                          ? std::string(reinterpret_cast<char*>(buffer.data()),
                                        new_len)
                          : std::string();

  std::printf("input %zu bytes, patched = %s\n", file.size(),
              patched ? "yes" : "no");
  if (!patched) {
    std::printf(
        "PatchSettingsHtml refused to write: no marker, no anchor, or the "
        "result would not fit.\n");
    return 1;
  }
  std::printf("banner injected = %s\n",
              Contains(after, "Chrome++ Next</a>") ? "yes" : "no");
  std::printf("hidden=\"true\" attributes: %zu before, %zu after\n",
              CountOccurrences(before, "hidden=\"true\""),
              CountOccurrences(after, "hidden=\"true\""));

  // Printing the start tags that carry each binding is the point of this mode:
  // it shows the markup a Chrome build actually uses, and what the patch turned
  // it into.
  constexpr std::string_view kBindings[] = {"showUpdateStatus_",
                                            "shouldShowIcons_"};
  for (std::string_view binding : kBindings) {
    std::printf("%.*s tags:\n", static_cast<int>(binding.size()),
                binding.data());
    const std::vector<std::string_view> before_tags =
        TagMentions(before, binding, 3);
    const std::vector<std::string_view> after_tags =
        TagMentions(after, binding, 3);
    for (std::string_view tag : before_tags) {
      std::printf("  before: %.*s\n", static_cast<int>(tag.size()),
                  tag.data());
    }
    if (before_tags.empty()) {
      std::printf("  before: <none>\n");
    }
    for (std::string_view tag : after_tags) {
      std::printf("  after : %.*s\n", static_cast<int>(tag.size()), tag.data());
    }
    if (after_tags.empty()) {
      std::printf("  after : <none: no tag binds it any more>\n");
    }
  }
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc >= 3 && std::wstring_view(argv[1]) == L"--extract") {
    const std::wstring out = argc >= 4 ? argv[3] : L"about_page.html";
    return ExtractAboutPage(argv[2], out);
  }
  if (argc >= 3 && std::wstring_view(argv[1]) == L"--file") {
    return CheckExtractedFile(argv[2]);
  }

  std::printf("built-in markup cases\n");
  RunBuiltInCases();
  std::printf("\n%s (%d failure(s))\n",
              failures == 0 ? "ALL CASES PASSED" : "FAILURES", failures);
  return failures == 0 ? 0 : 1;
}
