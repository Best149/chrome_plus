# Chrome++ Next
[![LICENSE](https://img.shields.io/badge/License-GPL--3.0--only-blue.svg?style=for-the-badge&logo=github "LICENSE")](https://github.com/smzhzy26/chrome_plus/blob/main/LICENSE) [![LAST COMMIT](https://img.shields.io/github/last-commit/smzhzy26/chrome_plus?color=blue&logo=github&style=for-the-badge "LAST COMMIT")](https://github.com/smzhzy26/chrome_plus/commits/main) [![STARS](https://img.shields.io/github/stars/smzhzy26/chrome_plus?color=brightgreen&logo=github&style=for-the-badge "STARS")](https://github.com/smzhzy26/chrome_plus/stargazers) ![SIZES](https://img.shields.io/github/languages/code-size/smzhzy26/chrome_plus?color=brightgreen&logo=github&style=for-the-badge "SIZES")

English | [简体中文](README.zh-CN.md)

Chrome++ Next is a `version.dll` injection project for Google Chrome. It is loaded alongside `chrome.exe` and augments browser behavior at startup with tab, hotkey, portable, command-line, and policy-related features.

## Overview
- Targets Google Chrome on Windows.
- Works by placing `version.dll` next to `chrome.exe`.
- Focuses on practical browser behavior changes instead of UI wrappers or extensions.
- Prioritizes capabilities that browser extensions cannot implement well, or that external tools do not solve cleanly.

## Support Policy
- Bug reports are accepted only for the latest stable Google Chrome.
- Other Chromium-based browsers may work, but they are not supported targets.
- Reporting requirements are enforced in the GitHub Issues templates.

## Download
- [Latest releases](https://github.com/smzhzy26/chrome_plus/releases)

## Installation
- Put `version.dll` in the same directory as `chrome.exe`.
- The recommended installation method is to use the [Chrome offline installer package](https://github.com/smzhzy26/chrome_installer), extract it twice, and use the unpacked Chrome program files directly.
- The project is intended for portable Chrome deployments. If you keep updater components or other Chrome remnants on the system, you are responsible for the resulting environment-specific behavior.
- If `version.dll` is not loaded correctly, you can try [setdll](https://github.com/smzhzy26/setdll/).

## Capability Overview
### Tab and bookmark behavior
- Double-click to close tabs.
- Right-click to close tabs, with `Shift` preserving the original menu.
- Keep the last tab from closing the browser window.
- Switch tabs with the mouse wheel over the tab strip.
- Switch tabs with the mouse wheel while holding the right mouse button.
- Activate a tab by resting the cursor on it.
- Open omnibox input or bookmarks in a new tab.
- Control new-tab detection through `new_tab_disable` and `new_tab_disable_name`.

### Hotkeys and input remapping
- Configure a boss key to hide and restore Chrome windows, and mute or restore audio along with those actions.
- Configure a translate hotkey.
- Remap hotkeys to other key combinations or Chrome command IDs through `keymapping`.

### Portable deployment and startup behavior
- Override `data_dir` and `cache_dir` for portable use.
- Append Chromium switches through `command_line`.
- Run commands or programs with `launch_on_startup` and `launch_on_exit`.

### Browser environment controls
- Ignore enterprise policies with `ignore_policies`.
- Enable the `win32k` fallback only when Chrome++ itself causes startup crashes.
- Suppress Chrome's false "out of date" upgrade notification on portable installs with `suppress_false_upgrade_notification`, on by default together with `--disable-features=OutdatedBuildDetector` in `command_line`; this is the recommended setup for a deliberately frozen old Chrome.
- Additional public options such as `show_password` remain documented in [`src/chrome++.ini`](src/chrome++.ini).

## Configuration Reference
- See [`src/chrome++.ini`](src/chrome++.ini) for the full public configuration surface.

## Development Notes

### Keeping the update UI quiet
`suppress_false_upgrade_notification=1` (the default) makes the installed-version probe read the running version, and `command_line` disables `OutdatedBuildDetector`; together they keep a deliberately frozen portable Chrome from showing "out of date / relaunch to update".

### Verifying the settings-page patch against a Chrome build
Chrome++ hides the `chrome://settings/help` update row by rewriting that page's HTML inside `resources.pak`, so it depends on the markup a given Chrome ships (the known binding names are listed in [`src/pakpatch.cc`](src/pakpatch.cc)). When moving to a new Chrome version, check it offline instead of guessing:

```powershell
cmake -S . -B build/tests -DCHROME_PLUS_BUILD_TESTS=ON   # off by default, never part of a release build
cmake --build build/tests --config MinSizeRel
ctest --test-dir build/tests -C MinSizeRel

# or directly, against any installed Chrome:
build/tests/MinSizeRel/chrome_plus_markup_test.exe --extract "<chrome>\resources.pak"
build/tests/MinSizeRel/chrome_plus_markup_test.exe --file about_page.html
```

`--file` prints the start tag that carries each binding before and after the patch. If it reports `<none: no tag binds it any more>` the row is hidden and nothing needs changing; otherwise add the name that version uses to the candidate lists in `src/pakpatch.cc` and re-run.

## License
- Versions 1.5.4 and earlier are licensed under MIT, with all rights reserved by [Shuax](https://github.com/shuax/).
- Versions 1.5.5 through 1.5.9 are licensed under MIT, with modifications by contributors in this repository based on Shuax's version.
- Versions 1.6.0 and later are licensed under [GPL-3.0](LICENSE).

## Thanks
- All [contributors](https://github.com/smzhzy26/chrome_plus/graphs/contributors)
- Original author [Shuax](https://github.com/shuax/)
- Revision code [provider](https://forum.ru-board.com/topic.cgi?forum=5&topic=51073&start=620&limit=1&m=1#1) for version 1.5.5
- [面向大海](https://github.com/mxdh/)
- [Ho Cheung](https://github.com/gz83/)
