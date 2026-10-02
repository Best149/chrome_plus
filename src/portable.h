#ifndef CHROME_PLUS_SRC_PORTABLE_H_
#define CHROME_PLUS_SRC_PORTABLE_H_

#include <windows.h>

#include <string>

// Relaunches chrome.exe with the portable command line built from the
// configuration. On success this process exits inside the call; returning at
// all means the relaunch failed and the failure has been logged.
void Portable(LPWSTR param);

#endif  // CHROME_PLUS_SRC_PORTABLE_H_
